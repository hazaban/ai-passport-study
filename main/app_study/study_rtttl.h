/*
 * study_rtttl.h — RTTTL (Nokia Ringing Tone Text Transfer Language) 解析+PCM合成
 *
 * RTTTL 参考语法:
 *   <name>:[d=<dur>][,o=<oct>][,b=<bpm>]:<note>[,<note>...]
 *   note = [<dur>][<pitch>][<oct>][.]   如 16g5, 8c6, 4a#, p
 *
 * 本实现兼容大部分常见 RTTTL，专注于 16kHz 16-bit mono PCM 方波合成，
 * 可直接调用 bsp_audio_write() 送入 ES8311（见设计文档 §3.4 & §4.4）。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------- 音符索引 ---------- */
typedef enum {
    NOTE_PAUSE = 0,   /* P / p */
    NOTE_C,  NOTE_C_SHARP,
    NOTE_D,  NOTE_D_SHARP,
    NOTE_E,
    NOTE_F,  NOTE_F_SHARP,
    NOTE_G,  NOTE_G_SHARP,
    NOTE_A,  NOTE_A_SHARP,
    NOTE_B,
} rtttl_pitch_t;

/* ---------- 默认参数 ---------- */
typedef struct {
    int default_duration;   /* 1,2,4,8,16,32 (分音符) */
    int default_octave;     /* 4..7 */
    int bpm;                /* 60..300 */
} rtttl_defaults_t;

/* ---------- 解析出的单个音符 ---------- */
typedef struct {
    rtttl_pitch_t note;
    int  octave;           /* 4..7 ，休止符时无意义 */
    int  duration_code;    /* 1=全分音符 2=二分 ... 32=32分 */
    bool dotted;           /* 附点：增加 1/2 时值 */
    int  freq;             /* Hz ，休止符=0，计算后填入 */
    int  ms;               /* 实际毫秒数，计算后填入 */
} rtttl_note_t;

/* ---------- 头部解析（name:defaults:notes） ---------- */
/* 返回 0 成功；*notes_ptr 指向 notes 部分的起始位置 */
int rtttl_parse_header(const char *rtttl,
                       rtttl_defaults_t *out_defs,
                       const char       **out_notes_ptr);

/* ---------- 音符迭代器 ---------- */
typedef struct {
    rtttl_defaults_t  defs;
    const char       *cursor;       /* 当前解析位置 */
    bool              done;
} rtttl_iter_t;

int  rtttl_iter_begin(rtttl_iter_t *it, const char *rtttl);
bool rtttl_iter_next(rtttl_iter_t *it, rtttl_note_t *out_note);

/* ---------- 频率表（公共，UI 预览也可用） ---------- */
/* octave = 4..7；返回 Hz 取整；NOTE_PAUSE 恒为 0 */
int  rtttl_note_freq(int octave, rtttl_pitch_t pitch);

/* 整曲总时长估算（毫秒）。用于调度器判断会响多久。
 * 解析失败返回 -1。 */
int  rtttl_total_duration_ms(const char *rtttl);

/* ---------- PCM 流式合成器（增量 render，省内存） ---------- */
#define RTTTL_PCM_AMPLITUDE   4096   /* 16bit 方波幅度，约 -18dBFS，避免破音又够响 */

typedef struct {
    rtttl_iter_t   it;
    int            sample_rate;      /* 16000 Hz，跟 bsp_audio_set_format 对齐 */
    int32_t        phase;            /* 当前方波相位计数（samples） */
    int32_t        half_period;      /* 半个周期的采样数（当前音符）；0=休止 */
    int32_t        remain_samples;   /* 当前音符剩余采样数 */
    bool           current_high;     /* 当前方波电平 */
    bool           song_done;
} rtttl_player_t;

/* 初始化播放器。失败（格式错）返回非 0 */
int  rtttl_player_begin(rtttl_player_t *p, const char *rtttl, int sample_rate);

/* 渲染最多 max_samples 个 16-bit PCM 采样到 out_buf。
 * 返回实际写入的样本数；整曲播放完毕返回 0。
 * 输出为单声道 16-bit 整数（小端，与 ES8311 I2S 格式一致）。 */
int  rtttl_player_render(rtttl_player_t *p, int16_t *out_buf, int max_samples);
