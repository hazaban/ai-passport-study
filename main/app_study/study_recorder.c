/*
 * study_recorder.c — 录音引擎（device-only，见 study_recorder.h）
 *
 * 录音与语音包同一套编解码：16kHz/单声道 Opus 裸流（u16LE 包长 + Opus 帧，20ms）。
 * capture 任务读麦克风(320 采样/帧) → opus 编码 → 写 /rec/ACTIVE.TMP；
 * 短按 OK/到 20min/空间不足 → 定稿为 /rec/R%07u.FRC。
 * 回放/导出同用 study_audio_codec 解码。
 */
#include "study_recorder.h"
#include "study_audio_codec.h"
#include "study_wifi.h"     /* 录音期间暂停 WiFi 释放堆 */
#include "bsp_audio.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>

static const char *TAG = "recorder";

#define REC_DIR     "/rec"
#define ACTIVE_TMP  "/rec/ACTIVE.TMP"

#define CAP_STACK_W  8192      /* 32KB，单位=字；静态栈，不从堆分配(libopus 首帧编码需大栈) */
#define PLAY_STACK   16384
#define REC_PRIO    5
#define CHK_EVERY   50

static bool s_inited = false;   /* 懒加载：只在进录音页时初始化，避免开机占用堆/阻塞 */
static int s_vol = 80;
static volatile bool s_recording = false;
static volatile bool s_paused = false;      /* 录音暂停(短按OK切换) */
static volatile bool s_playing = false;
static volatile bool s_stop_play = false;
static volatile bool s_rec_cmd_stop = false;
static volatile bool s_rec_cancel = false;
static TaskHandle_t s_cap_task = NULL;
static StackType_t s_cap_stk[CAP_STACK_W];   /* 静态栈：录音不占堆 */
static StaticTask_t s_cap_tcb;
static TaskHandle_t s_play_task = NULL;
static uint32_t s_seq = 1;
static uint32_t s_elapsed_ms = 0;

#define EVT_N 8
static volatile int s_evt[EVT_N], s_evt_w = 0, s_evt_r = 0;

static void push_evt(study_rec_evt_t e) {
    int w = s_evt_w;
    s_evt[w] = (int)e;
    w = (w + 1) % EVT_N;
    if (w == s_evt_r) s_evt_r = (s_evt_r + 1) % EVT_N;
    s_evt_w = w;
}

static uint32_t load_seq(void) {
    nvs_handle_t h;
    if (nvs_open("recorder", NVS_READWRITE, &h) == ESP_OK) {
        uint32_t v = 1;
        if (nvs_get_u32(h, "seq", &v) != ESP_OK) v = 1;
        nvs_close(h);
        return v;
    }
    return 1;
}
static void save_seq(uint32_t v) {
    nvs_handle_t h;
    if (nvs_open("recorder", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "seq", v);
        nvs_commit(h);
        nvs_close(h);
    }
}

void study_recorder_path(uint32_t seq, char *buf, size_t cap) {
    snprintf(buf, cap, "/rec/R%07lu.FRC", (unsigned long)seq);
}

void study_recorder_set_volume(int p) {
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    s_vol = p;
}
int study_recorder_volume(void) { return s_vol; }

bool study_recorder_active(void)       { return s_recording || s_playing; }
bool study_recorder_is_recording(void) { return s_recording; }
bool study_recorder_is_paused(void) { return s_paused; }
void study_recorder_toggle_pause(void) {
    if (!s_recording) return;
    s_paused = !s_paused;
    ESP_LOGI(TAG, "录音 %s", s_paused ? "暂停" : "继续");
}
bool study_recorder_is_playing(void)   { return s_playing; }

study_rec_evt_t study_recorder_poll_evt(void) {
    if (s_evt_r == s_evt_w) return REC_EVT_NONE;
    int e = s_evt[s_evt_r];
    s_evt_r = (s_evt_r + 1) % EVT_N;
    return (study_rec_evt_t)e;
}

int study_recorder_scan(study_rec_entry_t *o, int cap) {
    int n = 0;
    DIR *d = opendir(REC_DIR);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < cap) {
        const char *name = de->d_name;
        if (name[0] != 'R') continue;
        long seq = strtol(name + 1, NULL, 10);
        if (seq <= 0) continue;
        char path[64];
        study_recorder_path((uint32_t)seq, path, sizeof(path));
        study_frc_reader_t *rd = study_frc_open(path);
        if (!rd) continue;
        uint32_t fr = study_frc_num_frames(rd);
        study_frc_close(rd);
        o[n].seq = (uint32_t)seq;
        o[n].created_epoch = 0;
        o[n].ms_len = fr * 20;
        n++;
    }
    closedir(d);
    /* 最新在前 */
    for (int i = 0; i < n - 1; i++)
        for (int j = 0; j < n - 1 - i; j++)
            if (o[j].seq < o[j + 1].seq) {
                study_rec_entry_t t = o[j]; o[j] = o[j + 1]; o[j + 1] = t;
            }
    return n;
}

int study_recorder_free_kb(void) {
    size_t total = 0, used = 0;
    if (esp_spiffs_info("recordings", &total, &used) != ESP_OK) return -1;
    return (int)((total - used) / 1024);
}

/* ---------- 录音任务 ---------- */
static void cap_task(void *arg) {
    (void)arg;
    s_recording = true;
    s_rec_cmd_stop = false;
    s_rec_cancel = false;
    s_paused = false;
    study_wifi_pause();   /* 暂停 WiFi，释放堆给 Opus 编码 */
    vTaskDelay(pdMS_TO_TICKS(300));   /* 等 WiFi/LWIP 真正释放内存 */
    ESP_LOGI(TAG, "CAP: WiFi 已暂停(等300ms后), heap=%d", esp_get_free_heap_size());

    if (esp_get_free_heap_size() < 30000) {
        ESP_LOGE(TAG, "heap low(%d), 不录", esp_get_free_heap_size());
        push_evt(REC_EVT_AUDIO_ERR);
        study_wifi_resume(); s_recording = false; s_cap_task = NULL; vTaskDelete(NULL); return;
    }
    if (bsp_audio_set_format(STUDY_CODEC_RATE, 16, 1) != ESP_OK) {
        push_evt(REC_EVT_AUDIO_ERR);
        study_wifi_resume(); s_recording = false; s_cap_task = NULL; vTaskDelete(NULL); return;
    }
    bsp_audio_set_volume((uint8_t)s_vol);

    ESP_LOGI(TAG, "CAP 阶段: codec 16k 设置 OK, heap=%d", esp_get_free_heap_size());
    study_frc_writer_t *w = study_frc_create(ACTIVE_TMP);
    if (!w) {
        ESP_LOGE(TAG, "CAP 阶段: 建文件失败(存储满?)");
        push_evt(REC_EVT_STORAGE_FULL);
        study_wifi_resume(); s_recording = false; s_cap_task = NULL; vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "CAP 阶段: 录音文件 ACTIVE.TMP 创建 OK");

    int16_t buf[STUDY_FRAME_SAMPLES];
    uint32_t frames = 0;
    s_elapsed_ms = 0;
    bool autostop = false, full = false, ioerr = false;

    ESP_LOGI(TAG, "cap: 录音启动 frames=0 heap=%d", esp_get_free_heap_size());

    while (!s_rec_cmd_stop) {
        if (s_paused) {              /* 短按OK暂停：不读麦、不计时长 */
            s_elapsed_ms = frames * 20;
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        if (bsp_audio_read(buf, sizeof(buf)) != ESP_OK) { ioerr = true; break; }
        if (study_frc_enc_frame(w, buf, STUDY_FRAME_SAMPLES) != 0) { ioerr = true; break; }
        frames++;
        if (frames == 1) ESP_LOGI(TAG, "CAP 阶段: 首帧写入 OK → mic 有数据，正在录音");
        s_elapsed_ms = frames * 20;   /* 按有效录音帧数计时(暂停不计入) */
        if ((frames % CHK_EVERY) == 0) {
            if (frames * 20u >= (uint32_t)REC_MAX_SEC * 1000u) { autostop = true; break; }
            int kb = study_recorder_free_kb();
            if (kb >= 0 && kb < 350) { full = true; break; }
        }
    }

    uint32_t nf = study_frc_written_frames(w);
    if (s_rec_cancel || ioerr) {
        push_evt(ioerr ? REC_EVT_REC_ERR : REC_EVT_REC_CANCELLED);
        study_frc_abort(w);
        remove(ACTIVE_TMP);
    } else if (nf == 0) {
        study_frc_abort(w);
        remove(ACTIVE_TMP);
        push_evt(REC_EVT_REC_CANCELLED);
    } else {
        if (autostop) push_evt(REC_EVT_MAX_TIME);
        else if (full) push_evt(REC_EVT_STORAGE_FULL);
        if (study_frc_finalize(w) != 0) {
            remove(ACTIVE_TMP);
            push_evt(REC_EVT_REC_ERR);
        } else {
            uint32_t s = s_seq;
            char path[64];
            study_recorder_path(s, path, sizeof(path));
            uint32_t guard = 0;
            while (fopen(path, "rb") && guard < 100000) { s++; study_recorder_path(s, path, sizeof(path)); guard++; }
            if (rename(ACTIVE_TMP, path) == 0) {
                s_seq = s + 1; save_seq(s_seq);
                ESP_LOGI(TAG, "saved R%07lu (%lu f)", (unsigned long)s, (unsigned long)nf);
                push_evt(REC_EVT_REC_SAVED);
            } else {
                remove(ACTIVE_TMP);
                push_evt(REC_EVT_REC_ERR);
            }
        }
    }
    s_elapsed_ms = 0;
    study_wifi_resume();   /* 录完恢复 WiFi */
    s_recording = false;
    s_cap_task = NULL;
    vTaskDelete(NULL);
}

int study_recorder_start(void) {
    ESP_LOGI(TAG, "START 请求 rec=%d play=%d heap=%d", s_recording ? 1 : 0, s_playing ? 1 : 0,
             esp_get_free_heap_size());
    if (s_recording || s_playing || s_cap_task) {
        ESP_LOGW(TAG, "START 拒绝: recorder/play 已忙");
        return -1;
    }
    /* ESP-IDF 的栈大小按“字节”计；静态数组按“字(StackType_t)”分配 */
    s_cap_task = xTaskCreateStatic(cap_task, "rec_cap",
                                   (uint32_t)(CAP_STACK_W * sizeof(StackType_t)), NULL, REC_PRIO,
                                   s_cap_stk, &s_cap_tcb);
    if (!s_cap_task) {
        ESP_LOGE(TAG, "START 失败: xTaskCreateStatic(cap) 失败");
        push_evt(REC_EVT_REC_ERR);
        return -1;
    }
    push_evt(REC_EVT_REC_STARTED);
    ESP_LOGI(TAG, "START OK: cap 任务已创建");
    return 0;
}
int study_recorder_stop(void) {
    if (!s_recording) return -1;
    s_rec_cmd_stop = true;   /* capture 每帧轮询，很快退出保存 */
    return 0;
}
void study_recorder_cancel(void) {
    s_rec_cancel = true;
    s_rec_cmd_stop = true;
}
uint32_t study_recorder_elapsed_ms(void) { return s_elapsed_ms; }

/* ---------- 回放 ---------- */
static void play_task(void *arg) {
    char *path = (char *)arg;
    s_playing = true;
    s_stop_play = false;
    study_wifi_pause();
    if (bsp_audio_set_format(STUDY_CODEC_RATE, 16, 1) != ESP_OK) {
        study_wifi_resume(); free(path); push_evt(REC_EVT_PLAY_ERR); s_playing = false; s_play_task = NULL; vTaskDelete(NULL); return;
    }
    bsp_audio_set_volume((uint8_t)s_vol);

    study_frc_reader_t *rd = study_frc_open(path);
    free(path);
    if (!rd) { study_wifi_resume(); push_evt(REC_EVT_PLAY_ERR); s_playing = false; s_play_task = NULL; vTaskDelete(NULL); return; }

    int16_t pcm[512];
    int r = 0;
    while (!s_stop_play && (r = study_frc_read_pcm(rd, pcm, 512)) > 0) {
        if (bsp_audio_write(pcm, (size_t)r * 2) != ESP_OK) { r = -1; break; }
    }
    study_frc_close(rd);
    push_evt(r < 0 ? REC_EVT_PLAY_ERR : REC_EVT_PLAY_DONE);
    study_wifi_resume();
    s_playing = false;
    s_play_task = NULL;
    vTaskDelete(NULL);
}

int study_recorder_play_seq(uint32_t seq) {
    if (s_recording || s_playing) return -1;
    char *path = (char *)malloc(64);
    if (!path) return -1;
    study_recorder_path(seq, path, 64);
    if (xTaskCreate(play_task, "rec_play", PLAY_STACK, path, REC_PRIO,
                    (TaskHandle_t *)&s_play_task) != pdPASS) {
        free(path);
        return -1;
    }
    return 0;
}
void study_recorder_stop_playback(void) { s_stop_play = true; }

int study_recorder_delete_seq(uint32_t seq) {
    if (s_playing || s_recording) study_recorder_stop_playback();
    char path[64];
    study_recorder_path(seq, path, sizeof(path));
    if (remove(path) == 0) { push_evt(REC_EVT_DEL_DONE); return 0; }
    push_evt(REC_EVT_DEL_ERR);
    return -1;
}

int study_recorder_init(void) {
    if (s_inited) return 0;
    esp_vfs_spiffs_conf_t cf = {
        .base_path = REC_DIR,
        .partition_label = "recordings",
        .max_files = 8,
        .format_if_mount_failed = true,
    };
    if (esp_vfs_spiffs_register(&cf) != ESP_OK) {
        ESP_LOGE(TAG, "mount recordings failed");
        return -1;
    }
    size_t t = 0, u = 0;
    esp_spiffs_info("recordings", &t, &u);
    ESP_LOGI(TAG, "recordings %u/%u", (unsigned)u, (unsigned)t);
    s_seq = load_seq();
    remove(ACTIVE_TMP);
    s_inited = true;
    return 0;
}
