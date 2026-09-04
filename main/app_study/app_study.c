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
#include "opus.h"              /* Opus 解码(语音包 .frc) */
#include "study_recorder.h"    /* 录音机：生命周期 + busy 门闩 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_study";

static uint32_t s_last_long_ok_ms = 0;   /* 上次长按任意键的时间戳(ms)，用于抑制长按后的尾随/幽灵单击 */

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
/* 语音包现为 16kHz/单声道 Opus 裸流（.frc：每包 = u16LE 长度 + Opus 帧），
 * 参考钥匙扣项目：文件读取层实时 opus_decode → 只对 study_voice 吐 int16 PCM 字节。
 * 语音播放走独立 worker（串行），故这里只维护一个解码器实例。 */
static FILE         *s_voice_fp = NULL;
static OpusDecoder  *s_voice_dec = NULL;
static int16_t       s_voice_pend[960];
static int           s_voice_pend_n = 0;
static uint8_t       s_voice_pkt[1600];

static int voice_fs_open(const char *voice_key) {
    char path[96];
    int n = snprintf(path, sizeof(path), "/spiffs/%s.frc", voice_key);
    if (n < 0 || n >= (int)sizeof(path)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "VOICE open 失败(文件不存在): %s", path);
        return 0;
    }
    int err = 0;
    OpusDecoder *d = opus_decoder_create(VOICE_SAMPLE_RATE, 1, &err);
    if (err != OPUS_OK || !d) {
        ESP_LOGE(TAG, "VOICE open 失败(opus_decoder_create err=%d): %s", err, path);
        fclose(f);
        return 0;
    }
    s_voice_fp = f; s_voice_dec = d; s_voice_pend_n = 0;
    ESP_LOGI(TAG, "VOICE open OK: %s", path);
    return 1;
}
static int voice_fs_read(int handle, void *buf, int max) {
    (void)handle;
    if (!s_voice_dec || !s_voice_fp) return 0;
    int16_t *out = (int16_t *)buf;
    int need = max / 2;
    int got = 0;
    while (got < need) {
        if (s_voice_pend_n > 0) {
            int take = s_voice_pend_n > (need - got) ? (need - got) : s_voice_pend_n;
            memcpy(out + got, s_voice_pend, (size_t)take * 2);
            if (take < s_voice_pend_n) memmove(s_voice_pend, s_voice_pend + take,
                                               (size_t)(s_voice_pend_n - take) * 2);
            s_voice_pend_n -= take;
            got += take;
            continue;
        }
        uint8_t h[2];
        if (fread(h, 1, 2, s_voice_fp) != 2) break;          /* EOF */
        int plen = h[0] | (h[1] << 8);
        if (plen <= 0 || plen > (int)sizeof(s_voice_pkt)) break;
        if (fread(s_voice_pkt, 1, (size_t)plen, s_voice_fp) != (size_t)plen) break;
        int ns = opus_decode(s_voice_dec, s_voice_pkt, plen, s_voice_pend, 960, 0);
        if (ns < 0) {
            ESP_LOGE(TAG, "voice: opus_decode err %d (plen=%d)", ns, plen);
            break;
        }
        s_voice_pend_n = ns;
    }
    return got * 2;
}
static void voice_fs_close(int handle) {
    (void)handle;
    if (s_voice_dec) { opus_decoder_destroy(s_voice_dec); s_voice_dec = NULL; }
    if (s_voice_fp)  { fclose(s_voice_fp); s_voice_fp = NULL; }
    s_voice_pend_n = 0;
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
 * 这里给出 ESP32 强实现：探测 /spiffs/<key>.frc 是否真实存在。 */
bool study_voice_file_exists(const char *voice_key) {
    if (!voice_key) return false;
    char path[96];
    int n = snprintf(path, sizeof(path), "/spiffs/%s.frc", voice_key);
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

/* 编译时刻(__DATE__/__TIME__，按 UTC)换算成北京时间，作为出厂默认手动时钟。
 * 这样烧录后日期就是“今天”、时间接近当时，联网后 SNTP 再自动精确校准。 */
static void build_time_beijing(int *y, int *mo, int *d, int *h, int *mi) {
    static const char *mns = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mn[4] = {0};
    int dd = 0, yy = 0;
    sscanf(__DATE__, "%3s %d %d", mn, &dd, &yy);
    const char *p = strstr(mns, mn);
    int idx = p ? (int)(p - mns) / 3 : 0;
    int hh = 0, mm = 0, ss = 0;
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    hh += 8;                      /* UTC → 北京时间(UTC+8) */
    if (hh >= 24) { hh -= 24; dd += 1; }
    int leap = ((yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0) ? 1 : 0;
    static const int dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int mlen = dim[idx] + (idx == 1 ? leap : 0);
    if (dd > mlen) { dd = 1; idx++; if (idx >= 12) { idx = 0; yy++; } }
    *y = yy; *mo = idx + 1; *d = dd; *h = hh; *mi = mm;
}

/* 从 NVS 恢复手动时间（离线时日期/倒计时/提醒以此为准） */
static void restore_manual_time(void) {
    int y  = cfg_get("t_y", 0);
    int mo = cfg_get("t_mo", 0);
    int d  = cfg_get("t_d", 0);
    int h  = cfg_get("t_h", 0);
    int mi = cfg_get("t_mi", 0);
    if (y < 2000) {
        build_time_beijing(&y, &mo, &d, &h, &mi);   /* 出厂默认：烧录当天/时间 */
        cfg_set("t_y", y); cfg_set("t_mo", mo); cfg_set("t_d", d);
        cfg_set("t_h", h); cfg_set("t_mi", mi);
    }
    study_time_set_manual(y, mo, d, h, mi);
}

/* ---------- 洗头发：周期性（间隔天数可配置，默认 7 天） ----------
 * 完成洗头 → 记录当天 epoch 天数；之后第 interval 天早晨语音提醒。
 * 天数存 NVS(config.hair_last_day)，重启后仍能累计周期。 */
static void record_hair_wash(void) {
    long d = study_time_get_epoch_day();
    if (d < 0) return;                    /* 还没校时，无法定位第几天 → 跳过 */
    cfg_set("hair_last_day", (int)d);
    study_sched_hair_set_last_day(d);
}

/* ---------- UI callbacks 实现 ---------- */
static void on_task_done_changed(int task_id, bool done) {
    study_task_t t;
    if (study_task_get(task_id, &t) != 0) return;
    ESP_LOGI(TAG, "DONE 任务#%d '%s' done=%d category=%d", task_id, t.title, done ? 1 : 0, t.category);

    /* 洗头发特殊处理：OK 确认后不划线（不做完成标记），只记录本次洗头日，
     * 任务卡右上角随即自动显示下一次洗头时间（"还有X天"/"下次M/D"）。 */
    if (t.hour < 0 && strstr(t.title, "洗头发") != NULL) {
        if (!done) return;                     /* 洗头发无"取消完成"态 */
        record_hair_wash();
        if (bsp_lvgl_lock(100)) {
            ui_scene_show(SCENE_MSG_HAIR_WASH);
            bsp_lvgl_unlock();
        }
        voice_cmd_t cmd = { .kind = VCMD_SCENE };
        strncpy(cmd.key, "hair_wash", sizeof(cmd.key) - 1);
        xQueueSend(s_voice_cmd_q, &cmd, 0);
        return;
    }

    if (study_task_mark_done(task_id, done) != 0) return;
    if (!done) return;   /* 取消完成不弹鼓励 */

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
        ESP_LOGI(TAG, "COMPLETE 已入队 category=%d subtype=%d", t.category, t.subtype);
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

/* 白天/夜间模式切换：重建当前非首页页面，让新配色立即生效（首页保持独立亮色，不重建） */
static void on_theme_changed(int theme) {
    (void)theme;
    switch (s_page) {
        case PAGE_TODO:        ui_todo_destroy();       ui_todo_build();            break;
        case PAGE_ADD_TASK:    ui_add_destroy();        ui_add_build();             break;
        case PAGE_TASK_DETAIL: ui_detail_destroy();     ui_detail_build(ui_detail_current_id()); break;
        case PAGE_SETTINGS:    ui_settings_destroy();   ui_settings_build();        break;
        case PAGE_WIFI:        ui_wifi_destroy();       ui_wifi_build();            break;
        default: break;
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
    .on_theme_changed         = on_theme_changed,
};

/* ---------- 播放 worker 任务 ---------- */
static void voice_worker(void *arg) {
    (void)arg;
    voice_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_voice_cmd_q, &cmd, pdMS_TO_TICKS(200)) != pdPASS) continue;
        bool busy = study_recorder_active();
        bool haskey = cmd.key[0] != '\0';
        bool file = haskey && study_voice_file_exists(cmd.key);
        ESP_LOGI(TAG, "VOICE cmd kind=%d key='%s' | file存在=%d busy(录音/回放)=%d",
                 (int)cmd.kind, cmd.key, file ? 1 : 0, busy ? 1 : 0);
        if (busy) {
            ESP_LOGW(TAG, "VOICE dropped: recorder busy (录音=%d 回放=%d)",
                     study_recorder_is_recording() ? 1 : 0,
                     study_recorder_is_playing() ? 1 : 0);
            continue;
        }
        if (!file && (cmd.kind == VCMD_SCENE || cmd.kind == VCMD_COMPLETE)) {
            ESP_LOGW(TAG, "VOICE fallback: 语音文件缺失 key='%s' → 走 RTTTL 提示音", cmd.key);
        }
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

        /* 起床闹钟时间跟随设置页(wake_h/wake_m)变化，修改后立即生效 */
        study_sched_set_wake_time(cfg_get("wake_h", 7), cfg_get("wake_m", 0));

        /* 跨日复位每日任务与提醒状态 */
        if (ctm.tm_year >= 0) {   /* civil 可用(含手动) */
            int cd = (ctm.tm_year + 1900) * 10000 + (ctm.tm_mon + 1) * 100 + ctm.tm_mday;
            if (s_last_day > 0 && cd != s_last_day) {
                study_scheduler_new_day();
                clear_daily_done();
                /* 让当前 Todo 页立即显示“全部重置为未完成” */
                if (bsp_lvgl_lock(100)) {
                    ui_todo_refresh();
                    bsp_lvgl_unlock();
                }
            }
            s_last_day = cd;
        }

        study_sched_ev_t ev;
        /* ----- 1) 起床闹钟：到点只播温柔唤醒语音（无 RTTTL 铃声） ----- */
        if (study_sched_check_wakeup_alarm(now_h, now_m)) {
            voice_cmd_t cmd = { .kind = VCMD_SCENE };   /* 场景通路：直接播人声，无提示音 */
            strncpy(cmd.key, "wakeup_alarm", sizeof(cmd.key) - 1);
            xQueueSend(s_voice_cmd_q, &cmd, 0);
        }

        /* ----- 0) 洗头发：每 7 天早晨固定时刻语音提醒（需先联网校时） ----- */
        if (study_time_synced()) {
            long today_day = study_time_get_epoch_day();
            if (today_day > 0 && study_sched_hair_should_remind(today_day, now_h, now_m)) {
                voice_cmd_t cmd = { .kind = VCMD_SCENE };   /* 走场景语音通路：RTTTL+人声 */
                strncpy(cmd.key, "hair_remind", sizeof(cmd.key) - 1);
                xQueueSend(s_voice_cmd_q, &cmd, 0);
            }
        }

        /* ----- 2) 普通任务到点提醒 ----- */
        while (study_sched_find_next(now_h, now_m, &ev)
               && ev.overdue_minutes >= 0    /* 已到点或过期 */
               && ev.overdue_minutes < 10) { /* 10 分钟内的过期才响（避免开机补响几天前的）*/
            /* 1) 弹出提醒：柔和“叮咚”提示音 */
            voice_cmd_t cmd = { .kind = VCMD_RTTTL };
            strncpy(cmd.rtttl, STUDY_VOICE_CHIME_RTTTL, sizeof(cmd.rtttl) - 1);
            cmd.rtttl[sizeof(cmd.rtttl) - 1] = '\0';
            xQueueSend(s_voice_cmd_q, &cmd, 0);
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
    /* 起床闹钟时间从设置读取（默认 7:00；tick 循环也会跟随设置变化） */
    study_sched_set_wake_time(cfg_get("wake_h", 7), cfg_get("wake_m", 0));
    restore_manual_time();        /* 离线时可用的手动日期/时间 */
    ensure_daily_seed();          /* 只预置日常(旧版多余科目会被清理) */
    clear_daily_done();           /* 开机把重复任务 done 复位(新的一天) */

    /* 洗头发周期：重启后从 NVS 恢复上次洗头天数与间隔天数 */
    {
        int hd = cfg_get("hair_last_day", -1);
        if (hd > 0) study_sched_hair_set_last_day((long)hd);
        study_sched_hair_set_interval(cfg_get("hair_interval", STUDY_HAIR_INTERVAL_DEFAULT));
    }

    /* 2) voice 输出注入 + 语音文件系统挂载
     * 注：录音机不在此初始化(懒加载)，进录音页时才挂载 /rec，
     *     以保持开机语音路径与纯语音版(1999df6)完全一致。 */
    voice_fs_init();
    study_voice_set_output(audio_output_cb, audio_format_hint);
    s_voice_cmd_q = xQueueCreate(8, sizeof(voice_cmd_t));
    xTaskCreate(voice_worker, "study_voice", 16384, NULL, 4, &s_voice_task);

    /* 3) UI 初始化 & 载入首屏（封面主页面） */
    study_ui_init(&s_ui_cb);
    /* 应用持久化的屏幕亮度 */
    {
        int br = cfg_get("bright", 45);
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
    study_recorder_stop_playback();
    study_recorder_cancel();
    if (bsp_lvgl_lock(1000)) {
        switch (s_page) {
            case PAGE_HOME:       ui_home_destroy(); break;
            case PAGE_TODO:       ui_todo_destroy(); break;
            case PAGE_ADD_TASK:   ui_add_destroy(); break;
            case PAGE_TASK_DETAIL:ui_detail_destroy(); break;
            case PAGE_SETTINGS:   ui_settings_destroy(); break;
            case PAGE_WIFI:       ui_wifi_destroy(); break;
            case PAGE_RECORDER:   ui_rec_destroy(); break;
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
        case PAGE_RECORDER:    ui_rec_destroy(); break;
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
    if (!bsp_lvgl_lock(100)) return;   /* 超时从 500ms→100ms，避免按键回调被 LVGL 刷新长时间阻塞 */

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

    /* 记录任意键的“长按”时刻：三键共用一路 ADC，长按后的释放尾随/ADC 反弹会产生
       幽灵单击，若不抑制就会误触发任务选中（例如长按上/下切页签后立刻冒出一串选中）。 */
    if (ev == BSP_BTN_LONG) {
        s_last_long_ok_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
    /* 长按 OK = 返回上一页（封面页长按 = 退出回目录）。删除请用“双击 OK”。
     * 录音机页例外：长按 OK 在子屏内是「播放/停止/回设置」等专属语义。 */
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        if (s_page == PAGE_RECORDER) {
            ui_rec_key((uint8_t)btn, (uint8_t)ev);
            if (ui_rec_wants_home()) {
                ui_rec_destroy();
                s_page = PAGE_HOME;
                s_prev_page = PAGE_HOME;
                ui_home_build();
            } else if (ui_rec_wants_back()) {
                s_prev_page = PAGE_SETTINGS;
                ui_rec_destroy();
                ui_settings_build();
                s_page = PAGE_SETTINGS;
            }
            bsp_lvgl_unlock();
            return;
        }
        if (s_page == PAGE_HOME) s_wants_exit = true;
        else                     nav_back();
        bsp_lvgl_unlock();
        return;
    }
    /* 刚长按完 600ms 内的单击可能是释放尾随或 ADC 反弹，忽略，避免误触 */
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
            if (ui_settings_wants_recorder()) {
                s_prev_page = s_page;
                ui_settings_destroy();
                s_page = PAGE_RECORDER;
                ui_rec_enter();
            } else if (ui_settings_wants_wifi()) {
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
        case PAGE_RECORDER:
            ui_rec_key((uint8_t)btn, (uint8_t)ev);
            if (ui_rec_wants_home()) {
                ui_rec_destroy();
                s_page = PAGE_HOME;
                s_prev_page = PAGE_HOME;
                ui_home_build();
            } else if (ui_rec_wants_back()) {
                s_prev_page = PAGE_SETTINGS;
                ui_rec_destroy();
                ui_settings_build();
                s_page = PAGE_SETTINGS;
            }
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
