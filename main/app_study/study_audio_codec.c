/*
 * study_audio_codec.c — 8kHz/单声道 IMA-ADPCM 裸流编解码（录音专用，与语音包 Opus 链路彻底隔离）
 *
 * 本 codec 运行时堆占用约 200~500 B（FILE* + adpcm_state + 小 pkt 缓冲），
 * 不需要 Opus 的 ~120KB 伪栈，在 ESP32-C3 录音瞬间 ~60KB 空闲堆下完全不会 OOM。
 *
 * 算法参考：Intel/DVI IMA ADPCM 4-bit，标准 step table (89 entries) 与 index adjust 表。
 * 同一套表 + 同一套状态结构在 encoder/decoder 间共享，避免 study_voice.c 那份解码器
 * 与本文件之间的格式漂移（本文件的 decoder 只为录音回放服务，和 study_voice 无关）。
 */
#include "study_audio_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "codec";

/* FRC 文件头：magic + 4字节总帧数（小端），旧文件无此头则用 count_frames fallback */
#define FRC_MAGIC    0x46524331   /* "FRC1" */
#define FRC_HEADER_SZ 8

/* ---------- IMA-ADPCM 标准表（Flash 常量，不占堆） ---------- */
static const int16_t s_step_size[89] = {
     7,    8,    9,   10,   11,   12,   13,   14,   16,   17,
    19,   21,   23,   25,   28,   31,   34,   37,   41,   45,
    50,   55,   60,   66,   73,   80,   88,   97,  107,  118,
   130,  143,  157,  173,  190,  209,  230,  253,  279,  307,
   337,  371,  408,  449,  494,  544,  598,  658,  724,  796,
   876,  963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
  2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
  5894, 6484, 7132, 7845, 8630, 9493,10442,11487,12635,13899,
 15289,16818,18500,20350,22385,24623,27086,29794,32767
};
static const int8_t s_index_adjust[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

typedef struct {
    int32_t pred;       /* 上一个解码/编码得到的 PCM 预测值 */
    int     idx;        /* step_size 表索引 0..88 */
} adpcm_state_t;

/* ---------- 编解码核心 ---------- */

/* 编码一个样本 → 4-bit nibble。标准 IMA-ADPCM 差分公式。 */
static uint8_t adpcm_encode_sample(adpcm_state_t *s, int16_t sample) {
    int step = s_step_size[s->idx];
    int32_t diff = sample - s->pred;
    uint8_t nibble = 0;
    if (diff < 0) { nibble = 8; diff = -diff; }   /* sign bit */

    /* 标准 delta = step>>3 + (nibble&1)*step>>2 + (nibble&2)*step>>1 + (nibble&4)*step */
    int d = step >> 3;
    if (diff >= step)       { nibble |= 4; d += step; diff -= step; }
    step >>= 1;
    if (diff >= step)       { nibble |= 2; d += step; diff -= step; }
    step >>= 1;
    if (diff >= step)       { nibble |= 1; d += step; }
    if (nibble & 8) d = -d;

    s->pred += d;
    if (s->pred >  32767) s->pred =  32767;
    if (s->pred < -32768) s->pred = -32768;
    s->idx += s_index_adjust[nibble];
    if (s->idx < 0) s->idx = 0;
    if (s->idx > 88) s->idx = 88;
    return nibble;
}

/* 解码一个 4-bit nibble → int16。与 study_voice.c 那份逻辑完全一致（避免表漂移）。 */
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

/* ---------- 读/写帧头部 ---------- */

static void write_frc_payload(uint8_t *dst, adpcm_state_t *state,
                              const int16_t *pcm, int samples) {
    /* header: pred(i16LE) + idx(u8) + resv(u8=0) = 4 字节 */
    uint16_t pred = (uint16_t)(int16_t)state->pred;   /* 存 int16 截断值（IMA-ADPCM pred 应始终在 [-32768,32767] 内） */
    dst[0] = pred & 0xFF;
    dst[1] = (pred >> 8) & 0xFF;
    dst[2] = (uint8_t)(state->idx & 0xFF);
    dst[3] = 0;

    /* 压缩样本：IMA-ADPCM 每个字节存 2 个 nibble，低 nibble = 前一个样本 */
    int nibble_idx = 0;
    for (int i = 0; i < samples; i++) {
        adpcm_state_t tmp = *state;
        uint8_t nib = adpcm_encode_sample(&tmp, pcm[i]);
        /* 提交编码状态（encode_sample 已经在 tmp 上修改，这里回写） */
        state->pred = tmp.pred;
        state->idx  = tmp.idx;

        uint8_t *out_byte = dst + 4 + (nibble_idx >> 1);
        if ((nibble_idx & 1) == 0) {
            *out_byte = nib & 0x0F;      /* 低 4 bit = 先到的样本 */
        } else {
            *out_byte = (*out_byte & 0x0F) | ((nib & 0x0F) << 4);  /* 高 4 bit = 后到的样本 */
        }
        nibble_idx++;
    }
}

static int decode_frc_payload(const uint8_t *payload, adpcm_state_t *state,
                              int16_t *out, int max_samples) {
    /* 读 header：pred + idx + resv = 4 字节 */
    int16_t pred = (int16_t)((int)(payload[0]) | ((int)(payload[1]) << 8));
    state->pred = pred;
    state->idx  = (int)(payload[2] & 0xFF);
    if (state->idx < 0)  state->idx = 0;
    if (state->idx > 88) state->idx = 88;

    /* 解 80 字节压缩数据 → 160 个样本 */
    int got = 0;
    int total_nibbles = (STUDY_FRC_PAYLOAD_LEN - 4) * 2;   /* 80 字节 → 160 nibbles */
    for (int i = 0; i < total_nibbles && got < max_samples; i++) {
        uint8_t byte = payload[4 + (i >> 1)];
        uint8_t nibble = (i & 1) ? (byte >> 4) : (byte & 0x0F);
        out[got++] = adpcm_decode_sample(state, nibble);
    }
    return got;
}

/* ---------- study_frc_reader / writer 结构体 ---------- */

struct study_frc_reader {
    FILE         *fp;
    adpcm_state_t state;
    uint32_t      num_frames;
    int16_t       pending[STUDY_FRAME_SAMPLES];   /* 单帧 PCM 缓冲（小栈安全） */
    int           pending_n;
    uint8_t       pkt[STUDY_FRC_PAYLOAD_LEN];      /* 单帧 payload 读缓冲 */
};

struct study_frc_writer {
    FILE         *fp;
    adpcm_state_t state;
    uint32_t      num_frames;
};

/* ---------- 仅用于 open 预扫描帧数（纯 len 校验，不解析内容） ---------- */
static uint32_t count_frames(FILE *fp) {
    uint32_t n = 0;
    long pos = ftell(fp);
    uint8_t h[2];
    while (fread(h, 1, 2, fp) == 2) {
        int plen = h[0] | (h[1] << 8);
        /* 本 codec 固定 payload 长度 = 84；允许小范围兼容（老 Opus 流 len 变化范围不同） */
        if (plen != STUDY_FRC_PAYLOAD_LEN) break;
        if (fseek(fp, plen, SEEK_CUR) != 0) break;
        n++;
    }
    fseek(fp, pos, SEEK_SET);
    return n;
}

/* ---------- reader ---------- */

study_frc_reader_t *study_frc_open(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    study_frc_reader_t *r = (study_frc_reader_t *)calloc(1, sizeof(*r));
    if (!r) { fclose(fp); return NULL; }
    r->fp = fp;
    r->state.pred = 0;
    r->state.idx  = 0;
    /* 先尝试读新格式 magic header */
    uint32_t magic = 0, frames = 0;
    if (fread(&magic, 4, 1, fp) == 1 && magic == FRC_MAGIC &&
        fread(&frames, 4, 1, fp) == 1) {
        r->num_frames = frames;   /* 直接用头里的值，O(1) */
    } else {
        /* 旧格式（无 header）或 header 损坏：rewind 后遍历全文件 */
        fseek(fp, 0, SEEK_SET);
        r->num_frames = count_frames(fp);
    }
    ESP_LOGI(TAG, "ADPCM open OK: %s frames=%lu heap=%d",
             path, (unsigned long)r->num_frames, esp_get_free_heap_size());
    return r;
}

uint32_t study_frc_num_frames(const study_frc_reader_t *r) { return r->num_frames; }
uint32_t study_frc_rate(const study_frc_reader_t *r) { (void)r; return STUDY_CODEC_RATE; }

void study_frc_close(study_frc_reader_t *r) {
    if (!r) return;
    if (r->fp) fclose(r->fp);
    free(r);
}

int study_frc_read_pcm(study_frc_reader_t *r, int16_t *out, int max_samples) {
    if (!r) return -1;
    int got = 0;
    while (got < max_samples) {
        if (r->pending_n > 0) {
            int take = r->pending_n > (max_samples - got) ? (max_samples - got) : r->pending_n;
            memcpy(out + got, r->pending, (size_t)take * 2);
            if (take < r->pending_n) memmove(r->pending, r->pending + take,
                                             (size_t)(r->pending_n - take) * 2);
            r->pending_n -= take;
            got += take;
            continue;
        }
        uint8_t h[2];
        if (fread(h, 1, 2, r->fp) != 2) break;                 /* EOF */
        int plen = h[0] | (h[1] << 8);
        if (plen != STUDY_FRC_PAYLOAD_LEN) {
            ESP_LOGE(TAG, "read_pcm: payload len=%d expected=%d", plen, STUDY_FRC_PAYLOAD_LEN);
            return -1;
        }
        if (fread(r->pkt, 1, (size_t)plen, r->fp) != (size_t)plen) return -1;
        int ns = decode_frc_payload(r->pkt, &r->state, r->pending, STUDY_FRAME_SAMPLES);
        if (ns < 0) return -1;
        r->pending_n = ns;
    }
    return got;
}

/* ---------- writer ---------- */

study_frc_writer_t *study_frc_create(const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "create fopen 失败: %s (heap=%d)", path, esp_get_free_heap_size());
        return NULL;
    }
    /* 先写 8 字节 header：magic + 帧数占位（finalize 时回填） */
    uint32_t magic_placeholder = FRC_MAGIC;
    uint32_t frames_placeholder = 0;
    fwrite(&magic_placeholder, 4, 1, fp);
    fwrite(&frames_placeholder, 4, 1, fp);
    study_frc_writer_t *w = (study_frc_writer_t *)calloc(1, sizeof(*w));
    if (!w) { fclose(fp); ESP_LOGE(TAG, "create calloc 失败"); return NULL; }
    w->fp = fp;
    w->state.pred = 0;
    w->state.idx  = 0;
    ESP_LOGI(TAG, "ADPCM create OK: %s (heap=%d)", path, esp_get_free_heap_size());
    return w;
}

int study_frc_enc_frame(study_frc_writer_t *w, const int16_t *pcm, int samples) {
    if (!w || samples <= 0 || samples > STUDY_FRAME_SAMPLES) return -1;
    uint8_t hb[2];
    uint8_t payload[STUDY_FRC_PAYLOAD_LEN];

    write_frc_payload(payload, &w->state, pcm, samples);

    hb[0] = (uint8_t)(STUDY_FRC_PAYLOAD_LEN & 0xFF);
    hb[1] = (uint8_t)((STUDY_FRC_PAYLOAD_LEN >> 8) & 0xFF);
    if (fwrite(hb, 1, 2, w->fp) != 2) return -1;
    if (fwrite(payload, 1, STUDY_FRC_PAYLOAD_LEN, w->fp) != STUDY_FRC_PAYLOAD_LEN) return -1;
    w->num_frames++;
    return 0;
}

uint32_t study_frc_written_frames(const study_frc_writer_t *w) {
    return w ? w->num_frames : 0;
}

int study_frc_finalize(study_frc_writer_t *w) {
    if (!w) return -1;
    if (w->fp) {
        /* 回到文件头，回填真实帧数 */
        uint32_t magic = FRC_MAGIC;
        uint32_t frames = w->num_frames;
        rewind(w->fp);
        fwrite(&magic, 4, 1, w->fp);
        fwrite(&frames, 4, 1, w->fp);
        fclose(w->fp);
        w->fp = NULL;
    }
    free(w);
    return 0;
}

void study_frc_abort(study_frc_writer_t *w) {
    if (!w) return;
    if (w->fp) { fclose(w->fp); w->fp = NULL; }
    free(w);
}
