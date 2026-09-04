/*
 * study_audio_codec.c — 录音编解码（8kHz/单声道）
 *
 * 由 STUDY_REC_OPUS 选择编码：
 *   · Opus 窄带：SILK-only, complexity=0（绕开 libopus ~120KB 信号分析重路径）。
 *     编码器状态 ~20KB 堆 + 帧内临时缓冲可控，目标码率 ~0.75KB/s。
 *   · IMA-ADPCM：仅 ~200~500B 状态，固定 4KB/s，OOM 风险最低。
 *
 * 同用 FRC 容器：FRC2 头部带 codec_id，读端据此自描述解码；老 FRC1 文件仍按 ADPCM 读。
 */
#include "study_audio_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#if STUDY_REC_OPUS
#include "opus.h"
#endif
#if STUDY_REC_SPEEX
#include "speex/speex.h"
#endif

static const char *TAG = "codec";

/* FRC 头部：magic(4) + 帧数(4) [+ codec_id(1)，FRC2 时]。旧文件无 codec_id 则按 ADPCM。 */
#define FRC_MAGIC    0x46524331   /* "FRC1"：旧 ADPCM，无 codec_id */
#define FRC_MAGIC2   0x32435246   /* "FRC2"：新头部，含 codec_id */
#define FRC_HDR_SZ   8            /* FRC1 头大小 */
#define FRC_HDR2_SZ  9            /* FRC2 头大小：+1 codec_id */

/* Opus 每帧最长包：RFC6716 上限 1275B（任意 20ms 帧），留余量 */
#define OPUS_MAX_PKT 1400
/* Opus 解码输出最大样本（20ms帧@8k=160；留 40ms 兼容=320） */
#define OPUS_MAX_SAMPLES 480

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
    FILE   *fp;
    int     codec;                              /* STUDY_CODEC_ADPCM / OPUS / SPEEX */
    adpcm_state_t state;                        /* ADPCM 专用 */
    void    *dec;                               /* OPUS / SPEEX 解码器指针（共用） */
#if STUDY_REC_SPEEX
    SpeexBits bits;                             /* 复用位流 */
#endif
    uint32_t      num_frames;
    int16_t       pending[STUDY_FRAME_SAMPLES];
    int           pending_n;
    uint8_t       pkt[STUDY_FRC_PAYLOAD_LEN];   /* ADPCM 单帧读缓冲 */
    uint8_t       opkt[OPUS_MAX_PKT];           /* OPUS 单帧读缓冲 */
    uint8_t       spkt[STUDY_SPEEX_MAX_FRAME];  /* SPEEX 单帧读缓冲 */
};

struct study_frc_writer {
    FILE   *fp;
    int     codec;
    adpcm_state_t state;                        /* ADPCM 专用 */
    void    *enc;                               /* OPUS / SPEEX 编码器指针（共用） */
#if STUDY_REC_OPUS
    uint8_t     *opkt;                          /* OPUS 编码输出（calloc 一次） */
#endif
#if STUDY_REC_SPEEX
    SpeexBits bits;                             /* 复用位流 */
    uint8_t    spkt[STUDY_SPEEX_MAX_FRAME];
#endif
    uint32_t num_frames;
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
    r->codec = STUDY_CODEC_ADPCM;   /* 默认；FRC2 或 fallback 时再修正 */

    /* 先试 FRC2：magic + frames + codec_id。再试 FRC1：magic + frames（无 codec_id）。 */
    uint32_t magic = 0, frames = 0;
    if (fread(&magic, 4, 1, fp) == 1) {
        if (magic == FRC_MAGIC2 && fread(&frames, 4, 1, fp) == 1) {
            uint8_t cid = 0;
            if (fread(&cid, 1, 1, fp) == 1) {
                r->num_frames = frames;
                r->codec = STUDY_CODEC_ADPCM;
                if (cid == STUDY_CODEC_OPUS) r->codec = STUDY_CODEC_OPUS;
                else if (cid == STUDY_CODEC_SPEEX) r->codec = STUDY_CODEC_SPEEX;
            } else {
                fseek(fp, 0, SEEK_SET);
                r->codec = STUDY_CODEC_ADPCM;
                r->num_frames = count_frames(fp);
            }
        } else if (magic == FRC_MAGIC && fread(&frames, 4, 1, fp) == 1) {
            r->num_frames = frames;   /* FRC1：老 ADPCM，直接用头 */
        } else {
            /* 无有效头：按 ADPCM 遍历 */
            fseek(fp, 0, SEEK_SET);
            r->num_frames = count_frames(fp);
        }
    } else {
        fseek(fp, 0, SEEK_SET);
        r->num_frames = count_frames(fp);
    }

#if STUDY_REC_SPEEX
    if (r->codec == STUDY_CODEC_SPEEX) {
        int sr = STUDY_CODEC_RATE;
        r->dec = speex_decoder_init(&speex_nb_mode);
        if (!r->dec) {
            ESP_LOGE(TAG, "speex_decoder_init 失败 heap=%d", esp_get_free_heap_size());
            study_frc_close(r);
            return NULL;
        }
        speex_decoder_ctl(r->dec, SPEEX_SET_SAMPLING_RATE, &sr);
        speex_bits_init(&r->bits);
        ESP_LOGI(TAG, "Speex open OK: %s frames=%lu heap=%d",
                 path, (unsigned long)r->num_frames, esp_get_free_heap_size());
    } else
#endif
#if STUDY_REC_OPUS
    if (r->codec == STUDY_CODEC_OPUS) {
        int err = OPUS_OK;
        r->dec = opus_decoder_create(STUDY_CODEC_RATE, 1, &err);
        if (err != OPUS_OK || !r->dec) {
            ESP_LOGE(TAG, "opus_decoder_create err=%d", err);
            study_frc_close(r);
            return NULL;
        }
        ESP_LOGI(TAG, "Opus open OK: %s frames=%lu heap=%d",
                 path, (unsigned long)r->num_frames, esp_get_free_heap_size());
    } else
#endif
    {
        ESP_LOGI(TAG, "ADPCM open OK: %s frames=%lu heap=%d",
                 path, (unsigned long)r->num_frames, esp_get_free_heap_size());
    }
    return r;
}

uint32_t study_frc_num_frames(const study_frc_reader_t *r) { return r->num_frames; }
uint32_t study_frc_rate(const study_frc_reader_t *r) { (void)r; return STUDY_CODEC_RATE; }

void study_frc_close(study_frc_reader_t *r) {
    if (!r) return;
#if STUDY_REC_OPUS
    if (r->dec) { opus_decoder_destroy(r->dec); r->dec = NULL; }
#endif
#if STUDY_REC_SPEEX
    if (r->dec) { speex_decoder_destroy(r->dec); r->dec = NULL; }
    speex_bits_destroy(&r->bits);
#endif
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
#if STUDY_REC_SPEEX
        if (r->codec == STUDY_CODEC_SPEEX) {
            if (plen <= 0 || plen > STUDY_SPEEX_MAX_FRAME) {
                ESP_LOGE(TAG, "speex read: bad plen=%d", plen);
                return -1;
            }
            if (fread(r->spkt, 1, (size_t)plen, r->fp) != (size_t)plen) return -1;
            speex_bits_read_from(&r->bits, (const char *)r->spkt, plen);
            int ns = speex_decode_int(r->dec, &r->bits, r->pending);
            if (ns < 0) {
                ESP_LOGE(TAG, "speex_decode err %d (plen=%d)", ns, plen);
                return -1;
            }
            /* 注意：fixed-point 下 speex_decode_int 成功时返回 0（nb_decode 末行 return 0），
             * 真正的解码样本数是 NB 帧大小 STUDY_FRAME_SAMPLES，不能拿 ns 当代回样本数。 */
            r->pending_n = STUDY_FRAME_SAMPLES;
            continue;
        }
#endif
#if STUDY_REC_OPUS
        if (r->codec == STUDY_CODEC_OPUS) {
            if (plen <= 0 || plen > OPUS_MAX_PKT) {
                ESP_LOGE(TAG, "opus read: bad plen=%d", plen);
                return -1;
            }
            if (fread(r->opkt, 1, (size_t)plen, r->fp) != (size_t)plen) return -1;
            int ns = opus_decode(r->dec, r->opkt, plen, r->pending, STUDY_FRAME_SAMPLES, 0);
            if (ns < 0) {
                ESP_LOGE(TAG, "opus_decode err %d (plen=%d)", ns, plen);
                return -1;
            }
            r->pending_n = ns;
            continue;
        }
#endif
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
    study_frc_writer_t *w = (study_frc_writer_t *)calloc(1, sizeof(*w));
    if (!w) { fclose(fp); ESP_LOGE(TAG, "create calloc 失败"); return NULL; }
    w->fp = fp;
    w->state.pred = 0;
    w->state.idx  = 0;
#if STUDY_REC_SPEEX
    w->codec = STUDY_CODEC_SPEEX;
    /* Speex 窄带定点编码器：NB、Q3（~5.8kbps ≈ 0.73KB/s） */
    w->enc = speex_encoder_init(&speex_nb_mode);
    if (!w->enc) {
        ESP_LOGE(TAG, "speex_encoder_init 失败 heap=%d", esp_get_free_heap_size());
        study_frc_abort(w);
        return NULL;
    }
    {
        int q   = 3;                       /* 0-10；voice 常用 2-4 */
        int sr  = STUDY_CODEC_RATE;
        int vad = 0;
        int dtx = 0;
        speex_encoder_ctl(w->enc, SPEEX_SET_QUALITY, &q);
        speex_encoder_ctl(w->enc, SPEEX_SET_SAMPLING_RATE, &sr);
        speex_encoder_ctl(w->enc, SPEEX_SET_VAD, &vad);
        speex_encoder_ctl(w->enc, SPEEX_SET_DTX, &dtx);
    }
    speex_bits_init(&w->bits);
    ESP_LOGI(TAG, "Speex create OK: %s (heap=%d, q3 NB)", path, esp_get_free_heap_size());
#elif STUDY_REC_OPUS
    w->codec = STUDY_CODEC_OPUS;
    /* Opus 编码器：窄带 SILK-only、complexity=0（绕开 ~120KB 信号分析重路径）、VOIP */
    int err = OPUS_OK;
    w->enc = opus_encoder_create(STUDY_CODEC_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !w->enc) {
        ESP_LOGE(TAG, "opus_encoder_create err=%d (heap=%d)", err, esp_get_free_heap_size());
        study_frc_abort(w);
        return NULL;
    }
    opus_encoder_ctl(w->enc, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(w->enc, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_NARROWBAND));
    /* 编码输出缓冲 calloc 一次，避免每帧大栈 */
    w->opkt = (uint8_t *)calloc(1, OPUS_MAX_PKT);
    if (!w->opkt) {
        ESP_LOGE(TAG, "opus outbuf calloc 失败 (heap=%d)", esp_get_free_heap_size());
        study_frc_abort(w);
        return NULL;
    }
    ESP_LOGI(TAG, "Opus create OK: %s (heap=%d)", path, esp_get_free_heap_size());
#else
    w->codec = STUDY_CODEC_ADPCM;
    ESP_LOGI(TAG, "ADPCM create OK: %s (heap=%d)", path, esp_get_free_heap_size());
#endif
    /* 写新头部：FRC2 magic + frames 占位 + codec_id（finalize 回填帧数） */
    uint32_t magic = FRC_MAGIC2;
    uint32_t frames = 0;
    uint8_t cid = (uint8_t)w->codec;
    fwrite(&magic, 4, 1, w->fp);
    fwrite(&frames, 4, 1, w->fp);
    fwrite(&cid, 1, 1, w->fp);
    return w;
}

int study_frc_enc_frame(study_frc_writer_t *w, const int16_t *pcm, int samples) {
    if (!w || samples <= 0 || samples > STUDY_FRAME_SAMPLES) return -1;
    uint8_t hb[2];
    (void)hb;
#if STUDY_REC_SPEEX
    if (w->codec == STUDY_CODEC_SPEEX) {
        if (!w->enc) return -1;
        speex_bits_reset(&w->bits);
        if (speex_encode_int(w->enc, (spx_int16_t *)pcm, &w->bits) < 0) {
            ESP_LOGE(TAG, "speex_encode_int 失败");
            return -1;
        }
        speex_bits_insert_terminator(&w->bits);
        int n = speex_bits_write(&w->bits, (char *)w->spkt, STUDY_SPEEX_MAX_FRAME);
        if (n <= 0 || n > STUDY_SPEEX_MAX_FRAME) {
            ESP_LOGE(TAG, "speex 写帧失败 n=%d", n);
            return -1;
        }
        hb[0] = (uint8_t)(n & 0xFF);
        hb[1] = (uint8_t)((n >> 8) & 0xFF);
        if (fwrite(hb, 1, 2, w->fp) != 2) return -1;
        if (fwrite(w->spkt, 1, (size_t)n, w->fp) != (size_t)n) return -1;
        w->num_frames++;
        return 0;
    }
#endif
#if STUDY_REC_OPUS
    if (w->codec == STUDY_CODEC_OPUS) {
        if (!w->enc) return -1;
        int n = opus_encode(w->enc, (const opus_int16 *)pcm, samples,
                            w->opkt, OPUS_MAX_PKT);
        if (n < 0) {
            ESP_LOGE(TAG, "opus_encode err %d (samples=%d heap=%d)",
                     n, samples, esp_get_free_heap_size());
            return -1;
        }
        hb[0] = (uint8_t)(n & 0xFF);
        hb[1] = (uint8_t)((n >> 8) & 0xFF);
        if (fwrite(hb, 1, 2, w->fp) != 2) return -1;
        if (fwrite(w->opkt, 1, (size_t)n, w->fp) != (size_t)n) return -1;
        w->num_frames++;
        return 0;
    }
#endif
    {
        uint8_t payload[STUDY_FRC_PAYLOAD_LEN];
        write_frc_payload(payload, &w->state, pcm, samples);
        uint8_t h0[2];
        h0[0] = (uint8_t)(STUDY_FRC_PAYLOAD_LEN & 0xFF);
        h0[1] = (uint8_t)((STUDY_FRC_PAYLOAD_LEN >> 8) & 0xFF);
        if (fwrite(h0, 1, 2, w->fp) != 2) return -1;
        if (fwrite(payload, 1, STUDY_FRC_PAYLOAD_LEN, w->fp) != STUDY_FRC_PAYLOAD_LEN) return -1;
        w->num_frames++;
        return 0;
    }
}

uint32_t study_frc_written_frames(const study_frc_writer_t *w) {
    return w ? w->num_frames : 0;
}

int study_frc_finalize(study_frc_writer_t *w) {
    if (!w) return -1;
    if (w->fp) {
        /* 回到文件头，回填 FRC2 头：magic + 帧数 + codec_id */
        uint32_t magic = FRC_MAGIC2;
        uint32_t frames = w->num_frames;
        uint8_t cid = (uint8_t)w->codec;
        rewind(w->fp);
        fwrite(&magic, 4, 1, w->fp);
        fwrite(&frames, 4, 1, w->fp);
        fwrite(&cid, 1, 1, w->fp);
        fclose(w->fp);
        w->fp = NULL;
    }
#if STUDY_REC_SPEEX
    if (w->enc) { speex_encoder_destroy(w->enc); w->enc = NULL; }
    speex_bits_destroy(&w->bits);
#endif
#if STUDY_REC_OPUS
    if (w->enc) { opus_encoder_destroy(w->enc); w->enc = NULL; }
    if (w->opkt) { free(w->opkt); w->opkt = NULL; }
#endif
    free(w);
    return 0;
}

void study_frc_abort(study_frc_writer_t *w) {
    if (!w) return;
#if STUDY_REC_SPEEX
    if (w->enc) { speex_encoder_destroy(w->enc); w->enc = NULL; }
    speex_bits_destroy(&w->bits);
#endif
#if STUDY_REC_OPUS
    if (w->enc) { opus_encoder_destroy(w->enc); w->enc = NULL; }
    if (w->opkt) { free(w->opkt); w->opkt = NULL; }
#endif
    if (w->fp) { fclose(w->fp); w->fp = NULL; }
    free(w);
}
