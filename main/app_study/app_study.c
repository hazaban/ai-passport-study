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
#include "esp_task_wdt.h"      /* 挂载/格式化期间摘除看门狗，防止复位循环 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "app_study";

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
    bsp_audio_set_volume(80);
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
    /* 若 voicepack.bin 未烧入或分区未格式化，首次挂载会同步格式化整个分区。
     * 3.6MB 格式化耗时可能超过任务看门狗 5s → 看门狗复位 → 重启再格式化 → 无限复位循环。
     * 因此挂载期间暂时摘除当前任务看门狗，让格式化一次完成。 */
    esp_task_wdt_delete(NULL);            /* 若未注册会返回错误，忽略即可 */
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    esp_task_wdt_add(NULL);               /* 挂载结束立即恢复看门狗 */
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
    int last_fire_min = -1;
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(30 * 1000));  /* 每 30 秒粗轮询 */

        /* 时间源：SNTP 同步后优先用真实墙钟时间；未同步回落 NVS 手动时间 */
        int now_h, now_m;
        if (study_time_synced()) {
            study_time_get_now(&now_h, &now_m);
        } else {
            now_h = cfg_get("sim_h", 9);
            now_m = cfg_get("sim_m", 0);
        }
        (void)last_fire_min;

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

        /* ----- 1) 7:00 温柔唤醒闹钟（每天仅一次） ----- */
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

    /* 洗头发周期：重启后从 NVS 恢复上次洗头天数，保持每 7 天节奏 */
    {
        int hd = cfg_get("hair_last_day", -1);
        if (hd > 0) study_sched_hair_set_last_day((long)hd);
    }

    /* 2) voice 输出注入 + 语音文件系统挂载 */
    voice_fs_init();
    study_voice_set_output(audio_output_cb, audio_format_hint);
    s_voice_cmd_q = xQueueCreate(8, sizeof(voice_cmd_t));
    xTaskCreate(voice_worker, "study_voice", 4096, NULL, 4, &s_voice_task);

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
    xTaskCreate(tick_worker, "study_tick", 3072, NULL, 3, &s_tick_task);

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

    /* 如果有鼓励/场景弹窗，优先响应（按任意确认关闭） */
    if (ui_encourage_is_showing()) {
        if (ev == BSP_BTN_CLICK) ui_encourage_close();
        bsp_lvgl_unlock();
        return;
    }
    if (ui_scene_is_showing()) {
        if (ev == BSP_BTN_CLICK) ui_scene_close();
        bsp_lvgl_unlock();
        return;
    }

    /* 长按 OK = 返回上一页；封面页长按 = 完全退出回目录 */
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        if (s_page == PAGE_HOME) s_wants_exit = true;
        else                     nav_back();
        bsp_lvgl_unlock();
        return;
    }

    /* 双击快捷交互：
     * - 上/下 双击：在主页面集合中循环切换（PAGE_HOME -> PAGE_TODO -> PAGE_SETTINGS）
     * - 在 Todo 页内 OK 双击：快速将当前选中任务标记为完成
     */
    if (ev == BSP_BTN_DOUBLE) {
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            /* 仅在主页面集合间切换（避免进入编辑类子页被意外覆盖） */
            study_page_t order[] = { PAGE_HOME, PAGE_TODO, PAGE_SETTINGS };
            const int n = sizeof(order) / sizeof(order[0]);
            int idx = 0;
            for (int i = 0; i < n; i++) if (order[i] == s_page) { idx = i; break; }
            int next = idx + (btn == BSP_BTN_DOWN ? 1 : -1);
            if (next < 0) next = n - 1;
            if (next >= n) next = 0;
            study_page_t target = order[next];
            if (target != s_page) {
                /* 切页：销毁当前页并构建目标页 */
                switch (s_page) {
                    case PAGE_HOME: ui_home_destroy(); break;
                    case PAGE_TODO: ui_todo_destroy(); break;
                    case PAGE_SETTINGS: ui_settings_destroy(); break;
                    default: break;
                }
                switch (target) {
                    case PAGE_HOME: ui_home_build(); break;
                    case PAGE_TODO: ui_todo_build(); break;
                    case PAGE_SETTINGS: ui_settings_build(); break;
                    default: break;
                }
                s_prev_page = PAGE_HOME;
                s_page = target;
            }
            bsp_lvgl_unlock();
            return;
        }

        if (btn == BSP_BTN_OK && s_page == PAGE_TODO) {
            int sel = ui_todo_selected_task_id();
            if (sel >= 0) {
                /* 复用本文件的 on_task_done_changed 逻辑来统一标记并触发鼓励/语音 */
                on_task_done_changed(sel, true);
                ui_todo_refresh();
            }
            bsp_lvgl_unlock();
            return;
        }
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
                s_prev_page = s_page;
                ui_todo_destroy();
                ui_settings_build();
                s_page = PAGE_SETTINGS;
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
