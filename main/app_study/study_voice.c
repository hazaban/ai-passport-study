/*
 * study_voice.c — 播放编排 + 简易 IMA-ADPCM 解码框架
 *
 * 设计目标：在无 SPIFFS / 无硬件的纯宿主环境也能编译和运行（写 /dev/null），
 * 从而允许 UI + voice 编排代码的集成式编译验证。ESP32 端通过注入输出回调
 * + format_hint 接入 bsp_audio，通过 study_voice_file_exists 接入 SPIFFS。
 *
 * 为保证单文件的可移植性，不在这里直接 include "bsp_audio.h" / "esp_spiffs.h"，
 * 改用运行时注入函数指针的方式，由 app_study.c 在 init 时统一赋值。
 */
#include "study_voice.h"
#include "study_category.h"
#include "study_task.h"      /* CAT_* / SUBTYPE_* 枚举 */
#include "study_rtttl.h"
#include <string.h>

/* ---------- 语音 key 映射 ----------
 * subtype = -1 表示「该类通用」；英语下有 subtype 级的专属定制。 */
const study_voice_map_t study_voice_map[] = {
    /* 每日固定闹钟 */
    { -1, -1, "wakeup_alarm"  },
    /* 日常秩序场景 */
    { -1, -1, "morning_wash" },
    { -1, -1, "start_study"  },
    { -1, -1, "lunch"        },
    { -1, -1, "dinner"       },
    { -1, -1, "night_wash"   },
    { -1, -1, "sleep"        },
    { -1, -1, "hair_wash"    },

    /* 10 科「完成语」默认版 */
    { 0, -1, "complete_daily"    },
    { 1, -1, "complete_math"     },
    { 2, -1, "complete_linear"   },
    { 3, -1, "complete_prob"     },
    { 4, -1, "complete_ds"       },
    { 5, -1, "complete_co"       },
    { 6, -1, "complete_os"       },
    { 7, -1, "complete_network"  },
    { 8, -1, "complete_english"  },   /* 英语通用兜底 */
    { 9, -1, "complete_politics" },

    /* ★ 英语专属定制：subtype 不同 → 不同文案 & 语音 */
    { CAT_ENGLISH, SUBTYPE_RECITE,  "english_recite_words" },  /* 背单词完成 */
    { CAT_ENGLISH, SUBTYPE_CHAPTER, "eng_reading" },  /* 阅读题完成 */

    /* 自定义 */
    { -1, -1, "custom_1" },
    { -1, -1, "custom_2" },
    { -1, -1, "custom_3" },
    { -1, -1, "custom_4" },
    { -1, -1, "custom_5" },
};
const int study_voice_map_size =
    (int)(sizeof(study_voice_map) / sizeof(study_voice_map[0]));

const char *study_voice_resolve_key(int category, int subtype) {
    /* 1) 精确匹配 category + subtype 有定制 */
    for (int i = 0; i < study_voice_map_size; i++) {
        if (study_voice_map[i].category == category
                && study_voice_map[i].subtype == subtype) {
            return study_voice_map[i].voice_key;
        }
    }
    /* 2) 退而求其次：找该类「默认 complete_*」 */
    for (int i = 0; i < study_voice_map_size; i++) {
        if (study_voice_map[i].category == category
                && study_voice_map[i].subtype == -1) {
            return study_voice_map[i].voice_key;
        }
    }
    return NULL;
}

static const char *cat_to_voice_key(int category) {
    /* 旧 API 兼容：默认 subtype=-1 的通用版 */
    return study_voice_resolve_key(category, -1);
}

/* ---------- 运行时注入 ---------- */
static study_voice_output_cb_t s_out_cb;
static void (*s_format_hint)(void);
static int  s_volume = 80;        /* 默认 80% */
static bool (*s_file_exists_cb)(const char *key) = NULL;

void study_voice_set_output(study_voice_output_cb_t cb, void (*fh)(void)) {
    s_out_cb = cb;
    s_format_hint = fh;
}

void study_voice_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
}

/* 弱默认：宿主如果没给 file_exists，就一律视为不存在 → 走 RTTTL fallback */
__attribute__((weak)) bool study_voice_file_exists(const char *key) {
    (void)key;
    if (s_file_exists_cb) return s_file_exists_cb(key);
    return false;
}

/* ---------- 内核：把一段 int16 缓冲按音量缩放后推给输出回调 ---------- */
static void feed(int16_t *buf, int n) {
    if (!s_out_cb) return;
    if (s_volume != 100) {
        const int mult = (s_volume * 65536) / 100;
        for (int i = 0; i < n; i++) {
            int32_t s = (int32_t)buf[i] * mult >> 16;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            buf[i] = (int16_t)s;
        }
    }
    s_out_cb(buf, n);
}

/* ---------- 整段 RTTTL 同步播放（阻塞调用，ESP32 端会放在独立任务里） ---------- */
static void play_rtttl_blocking(const char *rtttl) {
    if (!rtttl) return;
    if (s_format_hint) s_format_hint();

    rtttl_player_t p;
    if (rtttl_player_begin(&p, rtttl, VOICE_SAMPLE_RATE) != 0) return;

    enum { CHUNK = 512 };
    int16_t buf[CHUNK];
    int got;
    while ((got = rtttl_player_render(&p, buf, CHUNK)) > 0) {
        feed(buf, got);
    }
}

/* ---------- ADPCM 解码（简易版） ----------
 * IMA-ADPCM 步进索引表（标准 4-bit）。若语音包只提供 PCM WAV，
 * 则该部分不会被调用；预留作 ADPCM 压缩语音时的解码通路。
 * 标准 Intel/DVI ADPCM index table 和 step table。 */
static const int16_t s_step_size[89] = {
    7,    8,    9,    10,   11,   12,   13,   14,   16,   17,
    19,   21,   23,   25,   28,   31,   34,   37,   41,   45,
    50,   55,   60,   66,   73,   80,   88,   97,   107,  118,
    130,  143,  157,  173,  190,  209,  230,  253,  279,  307,
    337,  371,  408,  449,  494,  544,  598,  658,  724,  796,
    876,  963,  1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442,11487,12635,13899,
    15289,16818,18500,20350,22385,24623,27086,29794,32767
};
static const int8_t s_index_adjust[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

typedef struct {
    int32_t pred;       /* 上一次预测的采样 */
    int     idx;        /* step_size 表索引 0..88 */
} adpcm_state_t;

static int16_t adpcm_decode_sample(adpcm_state_t *s, uint8_t nibble) {
    int step = s_step_size[s->idx];
    int32_t diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) diff = -diff;
    s->pred += diff;
    if (s->pred >  32767) s->pred =  32767;
    if (s->pred < -32768) s->pred = -32768;
    s->idx += s_index_adjust[nibble];
    if (s->idx < 0) s->idx = 0;
    if (s->idx > 88) s->idx = 88;
    return (int16_t)s->pred;
}

/* 播放一条语音。
 * 通过 study_voice_set_fs() 注入的 fs 提供 SPIFFS 读取；
 * 若未注入或文件缺失，调用方应走 RTTTL fallback。 */
static const study_voice_fs_t *s_fs = NULL;

void study_voice_set_fs(const study_voice_fs_t *fs) { s_fs = fs; }

/* 简易 WAV (PCM 16kHz 16bit mono) 流式解码并播放 */
static bool play_wav_blocking(const char *key) {
    if (!s_fs || !s_fs->open) return false;
    int h = s_fs->open(key);
    if (!h) return false;

    uint8_t header[44];
    int got = s_fs->read(h, header, sizeof(header));
    if (got < 44 || memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        s_fs->close(h); return false;
    }
    /* 确认是 PCM 16kHz 16bit mono (format=1, nCh=1, sRate=16000, bps=16) */
    int fmt   = header[20] | (header[21] << 8);
    int nch   = header[22] | (header[23] << 8);
    int rate  = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
    int bps   = header[34] | (header[35] << 8);
    if (fmt != 1 || nch != 1 || rate != VOICE_SAMPLE_RATE || bps != 16) {
        s_fs->close(h); return false;
    }
    if (s_format_hint) s_format_hint();

    enum { CHUNK = 512 };
    int16_t buf[CHUNK];
    for (;;) {
        int need = (int)sizeof(buf);
        int n = s_fs->read(h, buf, need);
        if (n <= 0) break;
        int samples = n / 2;
        if (samples > 0) feed(buf, samples);
        if (n < need) break;
    }
    s_fs->close(h);
    return true;
}

/* ---------- 高层 API ---------- */
/* 为了避免在没有 FreeRTOS 的宿主环境引入依赖，
 * "是否异步播放"交给上层：
 *   同步实现：立刻播放（阻塞）
 *   ESP32：app_study 启动独立 Task 时包装成非阻塞（见 app_study.c） */
static volatile bool s_playing = false;
static volatile bool s_stop    = false;

static int play_blocking_rtttl_then_voice(const char *rtttl, const char *voice_key) {
    if (s_playing) return -1;
    s_playing = true;
    s_stop = false;

    if (rtttl && *rtttl) {
        play_rtttl_blocking(rtttl);
    }
    if (!s_stop && voice_key && study_voice_file_exists(voice_key)) {
        play_wav_blocking(voice_key);
    }

    s_playing = false;
    return 0;
}

int study_voice_play_complete(int category_id) {
    return study_voice_play_complete_with_subtype(category_id, -1);
}

int study_voice_play_complete_with_subtype(int category_id, int subtype) {
    const study_category_t *cat = study_category_get(category_id);
    if (!cat) return -1;
    const char *vkey = study_voice_resolve_key(category_id, subtype);
    return play_blocking_rtttl_then_voice(cat->rtttl, vkey);
}

int study_voice_play_scene(const char *voice_key) {
    if (!voice_key) return -1;
    /* 场景类 fallback：走日常秩序类的 RTTTL 作为通用提示音 */
    const study_category_t *daily_cat = study_category_get(0);
    return play_blocking_rtttl_then_voice(daily_cat ? daily_cat->rtttl : NULL, voice_key);
}

int study_voice_play_rtttl(const char *rtttl) {
    return play_blocking_rtttl_then_voice(rtttl, NULL);
}

void study_voice_stop(void) {
    s_stop = true;
    s_playing = false;
}
