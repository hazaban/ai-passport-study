/*
 * study_recorder.h — 录音引擎（device-only）
 *
 * 硬件链：ES8311 麦克风(I2S RX) → 16kHz/单声道 → Opus 20ms 帧 → /rec/ACTIVE.TMP，
 * 停止时定稿为 /rec/R%07u.FRC（内部稳定序号存 NVS recorder:seq）。
 * 文件头存 created_epoch，UI/网页据此显示「日期时间 + 当日条数」。
 *
 * 回放：Opus 解码 → bsp_audio 直写；录音与回放互斥（study_recorder_active()），
 * app 层的 voice_worker 在 active 时丢弃语音命令，避免抢占 codec。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define REC_MAX_FILES   32
#define REC_MAX_SEC     20 * 60      /* 单条上限 20 分钟（与参考玩法一致） */

typedef struct {
    uint32_t seq;            /* Rxxxxxxx 序号 */
    uint32_t created_epoch;  /* 录音起始 unix 秒，0=未校时 */
    uint32_t ms_len;         /* 时长毫秒 */
} study_rec_entry_t;

typedef enum {
    REC_EVT_NONE = 0,
    REC_EVT_REC_STARTED,
    REC_EVT_REC_SAVED,       /* 已保存新录音 */
    REC_EVT_REC_CANCELLED,
    REC_EVT_MAX_TIME,        /* 到 20 分钟自动保存 */
    REC_EVT_STORAGE_FULL,
    REC_EVT_REC_ERR,
    REC_EVT_PLAY_DONE,
    REC_EVT_PLAY_ERR,
    REC_EVT_DEL_DONE,
    REC_EVT_DEL_ERR,
    REC_EVT_AUDIO_ERR,
} study_rec_evt_t;

/* 挂载 /rec（recordings 分区，format_if_mount_failed）并做崩溃恢复/加载序号。 */
int  study_recorder_init(void);
void study_recorder_set_volume(int p);
int  study_recorder_volume(void);

/* 扫描 /rec/R*.FRC，最新在前；返回条数 */
int  study_recorder_scan(study_rec_entry_t *o, int cap);
int  study_recorder_free_kb(void);

/* ---------- 录音 ---------- */
bool     study_recorder_is_recording(void);
bool     study_recorder_is_paused(void);
void     study_recorder_toggle_pause(void);   /* 短按OK：暂停/继续 */
int      study_recorder_start(void);       /* 0 成功 */
int      study_recorder_stop(void);        /* 停止并保存，0 成功 */
void     study_recorder_cancel(void);      /* 放弃当前段 */
uint32_t study_recorder_elapsed_ms(void);

/* ---------- 回放 ---------- */
bool study_recorder_is_playing(void);
int  study_recorder_play_seq(uint32_t seq);
void study_recorder_stop_playback(void);

int  study_recorder_delete_seq(uint32_t seq);

/* busy 门闩：录音中 或 回放中（供 voice_worker / tick 避让） */
bool study_recorder_active(void);

study_rec_evt_t study_recorder_poll_evt(void);

/* 构造路径："/rec/R%07u.FRC" */
void study_recorder_path(uint32_t seq, char *buf, size_t cap);
