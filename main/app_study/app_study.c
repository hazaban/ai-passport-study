/*
 * app_study.c — 考研助手主控：状态机 + 任务调度 + 播放任务 + 回调桥接
 *
 * 职责 (所有硬件相关集成集中在这里，避免 study_ui.c / study_voice.c 直接依赖 ESP BSP):
 *   1) 进入时：初始化 NVS → study_task_set_store → 注入 study_voice 输出回调到
 *      bsp_audio → 启动 FreeRTOS 定时 tick（1Hz 检查 scheduler next_due）+ 播放 worker
 *   2) 按键：收到按键后派给当前页的 ui_*_key
 *   3) 回调：study_ui 触发 on_task_done_changed → 调 scheduler 场景钩子 →
 *      决定是否显示场景鼓励弹窗 → 调 study_voice 播放
 *
 * 页面状态机：
 *   开机进入 PAGE_TODO
 *   PAGE_TODO:
 *     ·短按OK→选中任务→PAGE_TASK_DETAIL
 *     ·底部 +添加 →PAGE_ADD_TASK
 *     ·底部 设置  →PAGE_SETTINGS
 */
#include "app_study.h"
#include "study_ui.h"
#include "study_task.h"
#include "study_task_nvs.h"
#include "study_category.h"
#include "study_group.h"
#include "study_scheduler.h"
#include "study_voice.h"
#include "study_time.h"
#include "study_wifi.h"

#ifdef ESP_PLATFORM

#include "bsp_audio.h"
#include "bsp_display.h"       /* bsp_lvgl_lock/unlock */
#include "ui_pixel.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_spiffs.h"        /* 挂载 voicepack 分区读语音 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_study";

static uint32_t s_last_long_ok_ms = 0;   /* 上次长按OK时间戳(ms)，用于抑制尾随单击 */

static int  cfg_get(const char *k, int def);
static void cfg_set(const char *k, int v);

/* ---------- 页面状态 ---------- */
static study_page_t s_page = PAGE_HOME;
static study_page_t s_prev_page = PAGE_HOME;   /* 记录来源页，供长按返回 */
static bool s_wants_exit = false;              /* 封面页长按 OK = 完全退出回目录 */
static TaskHandle_t s_tick_task = NULL;
static TaskHandle_t s_voice_task = NULL;
static QueueHandle_t s_voice_cmd_q = NULL;

/* ---------- audio 输出回调：直接写 bsp_audio ---------- */
static void audio_format_hint(void) {
    bsp_audio_set_format(VOICE_SAMPLE_RATE, 16, 1);
    bsp_audio_set_volume(cfg_get("volume", 80));   /* 音量 0..100，可在设置改 */
}
static void audio_output_cb(const int16_t *buf, int num_samples) {
    bsp_audio_write(buf, (size_t)num_samples * 2);
}

/* ---------- SPIFFS 语音文件读取（注入给 study_voice） ---------- */
/* 约定：WAV 存于 /spiffs/<voice_key>.wav，16kHz/16bit/mono。 */
static int voice_fs_open(const char *voice_key) {
    char path[96];
    int n = snprintf(path, sizeof(path), "/spiffs/%s.wav", voice_key);
    if (n < 0 || n >= (int)sizeof(path)) return 0;
    FILE *f = fopen(path, "rb");
    return (int)(intptr_t)f;   /* NULL → 0 = 打开失败 */
}
static int voice_fs_read(int handle, void *buf, int max) {
    if (!handle) return 0;
    return (int)fread(buf, 1, (size_t)max, (FILE *)(intptr_t)handle);
}
static void voice_fs_close(int handle) {
    if (handle) fclose((FILE *)(intptr_t)handle);
}
static const study_voice_fs_t s_voice_fs = {
    .open  = voice_fs_open,
    .read  = voice_fs_read,
    .close = voice_fs_close,
};

/* 挂载 voicepack 分区(GSPIFFS)并注入语音读取回调；失败时语音走 RTTTL fallback */
static void voice_fs_init(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path            = "/spiffs",
        .partition_label      = "voicepack",
        .max_files            = 6,
        .format_if_mount_failed = true,   /* 首刷分区未格式化时自动格式化 */
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "voicepack SPIFFS 挂载失败(%s)，语音将回落到 RTTTL 提示音",
                 esp_err_to_name(err));
        study_voice_set_fs(NULL);
        return;
    }
    /* 确认分区存在，否则 SPIFFS 只有目录没有文件，播不出人声 */
    size_t total = 0, used = 0;
    esp_spiffs_info("voicepack", &total, &used);
    ESP_LOGI(TAG, "voicepack 已挂载: %u / %u bytes", (unsigned)used, (unsigned)total);
    study_voice_set_fs(&s_voice_fs);
}

/* study_voice.c 里的 study_voice_file_exists() 只是 weak 默认(恒返回 false)；
 * 这里给出 ESP32 强实现：探测 /spiffs/<key>.wav 是否真实存在。
 * 缺了它 play_wav_blocking() 永远不被调用 → 只有 RTTTL 旋律、人声静音。 */
bool study_voice_file_exists(const char *voice_key) {
    if (!voice_key) return false;
    char path[96];
    int n = snprintf(path, sizeof(path), "/spiffs/%s.wav", voice_key);
    if (n < 0 || n >= (int)sizeof(path)) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* ---------- voice_cmd：给播放 worker 的命令 ---------- */
typedef enum { VCMD_STOP, VCMD_COMPLETE, VCMD_SCENE, VCMD_RTTTL } vcmd_kind_t;
typedef struct {
    vcmd_kind_t kind;
    int         category;
    int         subtype;
    char        key[32];
    char        rtttl[160];
} voice_cmd_t;

static QueueHandle_t s_voice_cmd_q;

/* ---------- 配置回调：接 NVS config 命名空间 ---------- */
static int cfg_get(const char *k, int def) {
    extern const study_task_store_t *study_task_nvs_store(void);
    const study_task_store_t *s = study_task_nvs_store();
    if (!s || !s->load_meta_int) return def;
    return s->load_meta_int(k, def);
}
static void cfg_set(const char *k, int v) {
    extern const study_task_store_t *study_task_nvs_store(void);
    const study_task_store_t *s = study_task_nvs_store();
    if (s && s->save_meta_int) s->save_meta_int(k, v);
}

/* ---------- 首次/升级后：只保留“日常任务”预置（科目按需自己在添加页加） ----------
 * 旧版本曾把全部科目预置进列表，此处按 v2 清理掉非日常任务；若日常为空则重新预置。
 * 睡前洗漱 / 睡觉 不给固定时间(做成“待办”，手动点完成)。 */
static void ensure_daily_seed(void) {
    if (cfg_get("seed_v2", 0) != 0) return;
    int cap = TASK_MAX_COUNT;
    int *ids = (int *)malloc(sizeof(int) * cap);
    int daily = 0;
    if (ids) {
        int n = study_task_list_today(ids, cap);
        for (int i = 0; i < n; i++) {
            study_task_t t;
            if (study_task_get(ids[i], &t) != 0) continue;
            if (t.category == CAT_DAILY) daily++;
            else study_task_delete(ids[i]);       /* 清掉旧版预置的科目任务 */
        }
        free(ids);
    }
    if (daily == 0) {
        const study_preset_t *ps = NULL;
        int n = study_task_presets(&ps);
        for (int i = 0; i < n; i++) {
            if (ps[i].category != CAT_DAILY) continue;   /* 只预置日常 */
            study_task_t t;
            memset(&t, 0, sizeof(t));
            strncpy(t.title, ps[i].title, sizeof(t.title) - 1);
            t.title[sizeof(t.title) - 1] = '\0';
            t.category = ps[i].category;
            t.subtype  = ps[i].subtype;
            t.hour     = ps[i].hour;
            t.minute   = ps[i].minute;
            if (strstr(t.title, "睡前洗漱") != NULL || strstr(t.title, "睡觉") != NULL) {
                t.hour = -1; t.minute = -1;              /* 不给时间 */
            }
            t.repeat = STUDY_REPEAT_DAILY;               /* 每天一次，次日自动复位 */
            study_task_add(&t);
        }
    }
    cfg_set("seed_v2", 1);
    ESP_LOGI(TAG, "日常任务已就绪(科目按需添加)");
}

/* 开机/跨日：把“重复类”任务今天的 done 复位，便于新一天重新开始 */
static void clear_daily_done(void) {
    int cap = TASK_MAX_COUNT;
    int *ids = (int *)malloc(sizeof(int) * cap);
    if (!ids) return;
    int n = study_task_list_today(ids, cap);
    for (int i = 0; i < n; i++) {
        study_task_t t;
        if (study_task_get(ids[i], &t) != 0) continue;
        if (t.done && t.repeat != STUDY_REPEAT_ONCE) study_task_mark_done(ids[i], false);
    }
    free(ids);
}

/* 从 NVS 恢复手动时间（离线时日期/倒计时/提醒以此为准）。
 * 出厂/首次：直接写入“今天”日期(2026-09-03)作基础时钟，用户只需校准时分。 */
static void restore_manual_time(void) {
    int y  = cfg_get("t_y", 0);
    int mo = cfg_get("t_mo", 0);
    int d  = cfg_get("t_d", 0);
    int h  = cfg_get("t_h", 0);
    int mi = cfg_get("t_mi", 0);
    if (y < 2000) {
        y = 2026; mo = 9; d = 3; h = 0; mi = 0;   /* 今天的基础时钟 */
        cfg_set("t_y", y); cfg_set("t_mo", mo); cfg_set("t_d", d);
        cfg_set("t_h", h); cfg_set("t_mi", mi);
    }
    study_time_set_manual(y, mo, d, h, mi);
}

/* ---------- 洗头发：每 7 天周期 ----------
 * 完成洗头 → 记录当天 epoch 天数；之后第 7 天早晨语音提醒。
 * 天数存 NVS(config.hair_last_day)，重启后仍能累计周期。 */
static void record_hair_wash(void) {
    long d = study_time_get_epoch_day();
    if (d < 0) return;                    /* 还没校时，无法定位第几天 → 跳过 */
    cfg_set("hair_last_day", (int)d);
    study_sched_hair_set_last_day(d);
}

/* ---------- UI callbacks 实现 ---------- */
static void on_task_done_changed(int task_id, bool done) {
    if (study_task_mark_done(task_id, done) != 0) return;
    if (!done) return;   /* 取消完成不弹鼓励 */

    study_task_t t;
    if (study_task_get(task_id, &t) != 0) return;

    /* 日常秩序 → 场景钩子 */
    if (t.category == CAT_DAILY) {
        study_sched_scene_t scene;
        if (study_sched_scene_after_done(task_id, &scene)) {
            const char *vkey = NULL;
            study_scene_msg_t msg;
            switch (scene) {
                case STUDY_SCENE_MORNING_WASH: vkey = "morning_wash"; msg = SCENE_MSG_MORNING_WASH; break;
                case STUDY_SCENE_START_STUDY:  vkey = "start_study";  msg = SCENE_MSG_START_STUDY;  break;
                case STUDY_SCENE_LUNCH:        vkey = "lunch";        msg = SCENE_MSG_LUNCH;        break;
                case STUDY_SCENE_DINNER:       vkey = "dinner";       msg = SCENE_MSG_DINNER;       break;
                case STUDY_SCENE_NIGHT_WASH:   vkey = "night_wash";   msg = SCENE_MSG_NIGHT_WASH;   break;
                case STUDY_SCENE_SLEEP:        vkey = "sleep";        msg = SCENE_MSG_SLEEP;        break;
                case STUDY_SCENE_HAIR_WASH:
                    record_hair_wash();        /* 完成洗头 → 记当天，7 天后早晨会再提醒 */
                    vkey = "hair_wash";
                    msg  = SCENE_MSG_HAIR_WASH;
                    break;
                default: return;
            }
            if (bsp_lvgl_lock(100)) {
                ui_scene_show(msg);
                bsp_lvgl_unlock();
            }
            if (vkey) {
                voice_cmd_t cmd = { .kind = VCMD_SCENE };
                strncpy(cmd.key, vkey, sizeof(cmd.key) - 1);
                xQueueSend(s_voice_cmd_q, &cmd, 0);
            }
            return;
        }
    }

    /* 各科：显示鼓励 + 播放 RTTTL+语音 */
    const study_category_t *cat = study_category_get(t.category);
    if (cat) {
        if (bsp_lvgl_lock(100)) {
            ui_encourage_show(t.category);
            bsp_lvgl_unlock();
        }
        voice_cmd_t cmd = { .kind = VCMD_COMPLETE, .category = t.category, .subtype = t.subtype };
        xQueueSend(s_voice_cmd_q, &cmd, 0);
    }
}

static int on_task_added(const study_task_t *t) {
    study_task_t mut = *t;
    return study_task_add(&mut);   /* 会回填 id 和 created_at */
}

static int on_task_deleted(int task_id) {
    return study_task_delete(task_id);
}

static void play_complete_for_category(int category) {
    voice_cmd_t cmd = { .kind = VCMD_COMPLETE, .category = category, .subtype = -1 };
    xQueueSend(s_voice_cmd_q, &cmd, 0);
}
static void play_scene_if_daily(int task_id) {
    study_task_t t;
    if (study_task_get(task_id, &t) != 0 || t.category != CAT_DAILY) return;
    study_sched_scene_t scene;
    if (!study_sched_scene_after_done(task_id, &scene)) return;
    const char *vkey = NULL;
    switch (scene) {
        case STUDY_SCENE_MORNING_WASH: vkey = "morning_wash"; break;
        case STUDY_SCENE_START_STUDY:  vkey = "start_study";  break;
        case STUDY_SCENE_LUNCH:        vkey = "lunch";        break;
        case STUDY_SCENE_DINNER:       vkey = "dinner";       break;
        case STUDY_SCENE_NIGHT_WASH:   vkey = "night_wash";   break;
        case STUDY_SCENE_SLEEP:        vkey = "sleep";        break;
        case STUDY_SCENE_HAIR_WASH:    vkey = "hair_wash";    break;
    }
    if (vkey) {
        voice_cmd_t cmd = { .kind = VCMD_SCENE };
        strncpy(cmd.key, vkey, sizeof(cmd.key) - 1);
        xQueueSend(s_voice_cmd_q, &cmd, 0);
    }
}

static study_ui_callbacks_t s_ui_cb = {
    .on_task_done_changed     = on_task_done_changed,
    .on_task_added            = on_task_added,
    .on_task_deleted          = on_task_deleted,
    .play_complete_for_category = play_complete_for_category,
    .play_scene_if_daily      = play_scene_if_daily,
    .cfg_get                  = cfg_get,
    .cfg_set                  = cfg_set,
};

/* ---------- 播放 worker 任务 ---------- */
static void voice_worker(void *arg) {
    (void)arg;
    voice_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_voice_cmd_q, &cmd, pdMS_TO_TICKS(200)) != pdPASS) continue;
        study_voice_stop();
        switch (cmd.kind) {
            case VCMD_COMPLETE:
                study_voice_play_complete_with_subtype(cmd.category, cmd.subtype);
                break;
            case VCMD_SCENE:
                study_voice_play_scene(cmd.key);
                break;
            case VCMD_RTTTL:
                study_voice_play_rtttl(cmd.rtttl);
                break;
            default: break;
        }
    }
}

/* ---------- tick 任务：每分钟查 scheduler next_due ---------- */
static void tick_worker(void *arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    static int s_last_day = -1;
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(30 * 1000));  /* 每 30 秒粗轮询 */

        /* 时间源：SNTP 同步后优先真实墙钟；离线用手动时间(可设置) */
        struct tm ctm = {0};
        int now_h, now_m;
        if (study_time_civil_tm(&ctm)) {
            now_h = ctm.tm_hour;
            now_m = ctm.tm_min;
        } else {
            now_h = cfg_get("sim_h", 9);
            now_m = cfg_get("sim_m", 0);
        }

        /* 起床闹钟时间从设置读取；跨日复位每日任务与提醒状态 */
        study_sched_set_wake_time(cfg_get("wake_h", 7), cfg_get("wake_m", 0));
        if (ctm.tm_year >= 0) {   /* civil 可用(含手动) */
            int cd = (ctm.tm_year + 1900) * 10000 + (ctm.tm_mon + 1) * 100 + ctm.tm_mday;
            if (s_last_day > 0 && cd != s_last_day) {
                study_scheduler_new_day();
                clear_daily_done();
            }
            s_last_day = cd;
        }

        study_sched_ev_t ev;
        /* ----- 0) 洗头发：每 7 天早晨固定时刻语音提醒（需先联网校时） ----- */
        if (study_time_synced()) {
            long today_day = study_time_get_epoch_day();
            if (today_day > 0 && study_sched_hair_should_remind(today_day, now_h, now_m)) {
                voice_cmd_t cmd = { .kind = VCMD_SCENE };   /* 走场景语音通路：RTTTL+人声 */
                strncpy(cmd.key, "hair_remind", sizeof(cmd.key) - 1);
                xQueueSend(s_voice_cmd_q, &cmd, 0);
            }
        }

        /* ----- 1) 温柔唤醒闹钟（时间可在设置改，每天仅一次） ----- */
        if (study_sched_check_wakeup_alarm(now_h, now_m)) {
            /* 闹钟专属：轻柔 RTTTL + 温柔唤醒语音 */
            static const char wakeup_rtttl[] =
                "WakeUp:d=4,o=5,b=66:c5,g4,c5,e5,g5,c6,e6,p,g5,e5,c5";
            voice_cmd_t cmd = { .kind = VCMD_SCENE };
            /* 先走 RTTTL 轻柔前奏 + 再接 wakeup_alarm 温柔语音 */
            study_sched_ack_fired(0);  /* no-op */
            cmd.rtttl[0] = '\0';
            strncpy(cmd.key, "wakeup_alarm", sizeof(cmd.key) - 1);
            xQueueSend(s_voice_cmd_q, &cmd, 0);
            /* 用独立 RTTTL 指令作为前奏（RING 队列） */
            voice_cmd_t cmd_r = { .kind = VCMD_RTTTL };
            strncpy(cmd_r.rtttl, wakeup_rtttl, sizeof(cmd_r.rtttl) - 1);
            xQueueSendToFront(s_voice_cmd_q, &cmd_r, 0);
        }

        /* ----- 2) 普通任务到点提醒 ----- */
        while (study_sched_find_next(now_h, now_m, &ev)
               && ev.overdue_minutes >= 0    /* 已到点或过期 */
               && ev.overdue_minutes < 10) { /* 10 分钟内的过期才响（避免开机补响几天前的）*/
            /* 1) 弹出提醒（RTTTL + 屏幕大字） */
            const study_category_t *cat = NULL;
            study_task_t t;
            if (study_task_get(ev.task_id, &t) == 0) {
                cat = study_category_get(t.category);
            }
            if (cat) {
                voice_cmd_t cmd = { .kind = VCMD_RTTTL };
                strncpy(cmd.rtttl, cat->rtttl, sizeof(cmd.rtttl) - 1);
                xQueueSend(s_voice_cmd_q, &cmd, 0);
            }
            study_sched_ack_fired(ev.task_id);
            break;  /* 一次 tick 只处理一条，避免连响 N 次扰民 */
        }
    }
}

/* ---------- DEMO 入口 ---------- */
void app_study_enter(void) {
    /* 0) 联网 + 时间源：先连 WiFi（有凭证直连 / 无凭证自动开配网热点），
     *    联网后 SNTP 才能校时；未联网回落手动时间 */
    study_wifi_init("STU_STUDY");
    study_time_init();

    /* 1) NVS 存储层 */
    if (study_task_nvs_init() == 0) {
        const study_task_store_t *s = study_task_nvs_store();
        if (s) study_task_set_store(s);
    }
    study_scheduler_reset();
    restore_manual_time();        /* 离线时可用的手动日期/时间 */
    ensure_daily_seed();          /* 只预置日常(旧版多余科目会被清理) */
    clear_daily_done();           /* 开机把重复任务 done 复位(新的一天) */

    /* 洗头发周期：重启后从 NVS 恢复上次洗头天数，保持每 7 天节奏 */
    {
        int hd = cfg_get("hair_last_day", -1);
        if (hd > 0) study_sched_hair_set_last_day((long)hd);
    }

    /* 2) voice 输出注入 + 语音文件系统挂载 */
    voice_fs_init();
    study_voice_set_output(audio_output_cb, audio_format_hint);
    s_voice_cmd_q = xQueueCreate(8, sizeof(voice_cmd_t));
    xTaskCreate(voice_worker, "study_voice", 8192, NULL, 4, &s_voice_task);

    /* 3) UI 初始化 & 载入首屏（封面主页面） */
    study_ui_init(&s_ui_cb);
    /* 应用持久化的屏幕亮度 */
    {
        int br = cfg_get("bright", 80);
        bsp_display_backlight((uint8_t)br);
    }
    s_page = PAGE_HOME;
    s_prev_page = PAGE_HOME;
    s_wants_exit = false;
    if (bsp_lvgl_lock(1000)) {
        ui_home_build();
        bsp_lvgl_unlock();
    }

    /* 4) tick 任务 (调度器) */
    xTaskCreate(tick_worker, "study_tick", 8192, NULL, 3, &s_tick_task);

    ESP_LOGI(TAG, "考研助手已启动");
}

void app_study_exit(void) {
    if (s_tick_task)  { vTaskDelete(s_tick_task);  s_tick_task = NULL; }
    if (s_voice_task) { vTaskDelete(s_voice_task); s_voice_task = NULL; }
    if (s_voice_cmd_q) { vQueueDelete(s_voice_cmd_q); s_voice_cmd_q = NULL; }
    study_voice_stop();
    if (bsp_lvgl_lock(1000)) {
        switch (s_page) {
            case PAGE_HOME:       ui_home_destroy(); break;
            case PAGE_TODO:       ui_todo_destroy(); break;
            case PAGE_ADD_TASK:   ui_add_destroy(); break;
            case PAGE_TASK_DETAIL:ui_detail_destroy(); break;
            case PAGE_SETTINGS:   ui_settings_destroy(); break;
            case PAGE_WIFI:       ui_wifi_destroy(); break;
            default: break;
        }
        bsp_lvgl_unlock();
    }
    s_page = PAGE_HOME;
    s_prev_page = PAGE_HOME;
    s_wants_exit = false;
    ESP_LOGI(TAG, "考研助手已退出");
}

/* ---------- 长按返回：销毁当前页并重建上一页 ---------- */
static void nav_back(void) {
    study_page_t target = s_prev_page;
    if (target != PAGE_HOME && target != PAGE_TODO && target != PAGE_SETTINGS)
        target = PAGE_HOME;

    switch (s_page) {
        case PAGE_ADD_TASK:    ui_add_destroy(); break;
        case PAGE_TASK_DETAIL: ui_detail_destroy(); break;
        case PAGE_SETTINGS:    ui_settings_destroy(); break;
        case PAGE_WIFI:        ui_wifi_destroy(); break;
        case PAGE_TODO:        ui_todo_destroy(); break;
        default: break;
    }
    switch (target) {
        case PAGE_TODO:     ui_todo_build(); break;
        case PAGE_SETTINGS: ui_settings_build(); break;
        default:            ui_home_build(); target = PAGE_HOME; break;
    }
    s_prev_page = PAGE_HOME;
    s_page = target;
}

void app_study_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (!bsp_lvgl_lock(500)) return;

    /* 有鼓励/场景弹窗（完成语音正在播）时：短按/长按 OK 都=停止语音并关闭弹窗 */
    if (ui_encourage_is_showing()) {
        if (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG) {
            study_voice_stop();
            ui_encourage_close();
        }
        bsp_lvgl_unlock();
        return;
    }
    if (ui_scene_is_showing()) {
        if (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG) {
            study_voice_stop();
            ui_scene_close();
        }
        bsp_lvgl_unlock();
        return;
    }

    /* 长按 OK = 返回上一页（封面页长按 = 退出回目录）。删除请用“双击 OK”。 */
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        s_last_long_ok_ms = (uint32_t)(esp_timer_get_time() / 1000);  /* 抑制长按后的尾随单击 */
        if (s_page == PAGE_HOME) s_wants_exit = true;
        else                     nav_back();
        bsp_lvgl_unlock();
        return;
    }
    /* 刚长按完 600ms 内的单击可能是释放尾随，忽略，避免误触 */
    if (ev == BSP_BTN_CLICK && (esp_timer_get_time() / 1000) - s_last_long_ok_ms < 600) {
        bsp_lvgl_unlock();
        return;
    }

    switch (s_page) {
        case PAGE_HOME:
            ui_home_key((uint8_t)btn, (uint8_t)ev);
            if (ui_home_wants_study()) {
                s_prev_page = s_page;
                ui_home_destroy();
                ui_todo_build();
                s_page = PAGE_TODO;
            } else if (ui_home_wants_settings()) {
                s_prev_page = s_page;
                ui_home_destroy();
                ui_settings_build();
                s_page = PAGE_SETTINGS;
            }
            break;
        case PAGE_TODO: {
            ui_todo_key((uint8_t)btn, (uint8_t)ev);
            int tid = -1;
            if (ui_todo_wants_detail(&tid)) {
                s_prev_page = s_page;
                ui_todo_destroy();
                ui_detail_build(tid);
                s_page = PAGE_TASK_DETAIL;
            } else if (ui_todo_wants_add()) {
                s_prev_page = s_page;
                ui_todo_destroy();
                ui_add_build();
                s_page = PAGE_ADD_TASK;
            } else if (ui_todo_wants_settings()) {
                /* Todo 底部第二项 = 回“主页面”(封面)，避免与封面入口重复 */
                s_prev_page = s_page;
                ui_todo_destroy();
                ui_home_build();
                s_page = PAGE_HOME;
            } else if (ui_todo_wants_toggle(&tid) && tid > 0) {
                /* OK 单击：第一次=完成(鼓励+语音)，第二次=取消(不发声)。当天每个任务一次。 */
                study_task_t tt;
                if (study_task_get(tid, &tt) == 0) {
                    on_task_done_changed(tid, !tt.done);
                }
                ui_todo_refresh();
            } else if (ui_todo_wants_delete(&tid) && tid > 0) {
                on_task_deleted(tid);          /* 确认删除：从存储移除并刷新 */
                ui_todo_refresh();
            }
            break;
        }
        case PAGE_ADD_TASK: {
            int new_id = -1;
            ui_add_key((uint8_t)btn, (uint8_t)ev);
            if (ui_add_is_finished(&new_id)) {
                s_prev_page = s_page;
                ui_add_destroy();
                ui_todo_build();
                s_page = PAGE_TODO;
            }
            break;
        }
        case PAGE_TASK_DETAIL:
            ui_detail_key((uint8_t)btn, (uint8_t)ev);
            if (ui_detail_wants_back()) {
                s_prev_page = s_page;
                ui_detail_destroy();
                ui_todo_build();
                s_page = PAGE_TODO;
            }
            break;
        case PAGE_SETTINGS:
            ui_settings_key((uint8_t)btn, (uint8_t)ev);
            if (ui_settings_wants_wifi()) {
                s_prev_page = s_page;
                ui_settings_destroy();
                ui_wifi_build();
                s_page = PAGE_WIFI;
            } else if (ui_settings_wants_home()) {
                s_prev_page = s_page;
                ui_settings_destroy();
                ui_home_build();
                s_page = PAGE_HOME;
            } else if (ui_settings_wants_todo()) {
                s_prev_page = s_page;
                ui_settings_destroy();
                ui_todo_build();
                s_page = PAGE_TODO;
            }
            break;
        case PAGE_WIFI:
            ui_wifi_key((uint8_t)btn, (uint8_t)ev);
            break;
        default: break;
    }

    bsp_lvgl_unlock();
}

bool app_study_wants_exit(void) {
    bool r = s_wants_exit;
    s_wants_exit = false;
    return r;
}

#else  /* !ESP_PLATFORM — 宿主编译存根：方便静态语法检查 */

void app_study_enter(void) {}
void app_study_exit(void) {}
void app_study_key(bsp_btn_t btn, bsp_btn_ev_t ev) { (void)btn; (void)ev; }

#endif
