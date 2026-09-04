/*
 * study_audio_codec.c — 16kHz/单声道 Opus 裸流编解码（与语音包/钥匙扣一致）
 */
#include "study_audio_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opus.h"

struct study_frc_reader {
    FILE         *fp;
    OpusDecoder  *dec;
    uint32_t      num_frames;
    int16_t       pending[960];
    int           pending_n;
    uint8_t       pkt[1600];
};

struct study_frc_writer {
    FILE         *fp;
    OpusEncoder  *enc;
    uint32_t      num_frames;
    uint8_t       pkt[1600];
};

/* 仅用于 open 预扫描帧数 */
static uint32_t count_frames(FILE *fp) {
    uint32_t n = 0;
    long pos = ftell(fp);
    uint8_t h[2];
    while (fread(h, 1, 2, fp) == 2) {
        int plen = h[0] | (h[1] << 8);
        if (plen <= 0 || plen > 1600) break;
        if (fseek(fp, plen, SEEK_CUR) != 0) break;
        n++;
    }
    fseek(fp, pos, SEEK_SET);
    return n;
}

study_frc_reader_t *study_frc_open(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    int err = 0;
    OpusDecoder *dec = opus_decoder_create(STUDY_CODEC_RATE, 1, &err);
    if (err != OPUS_OK || !dec) { fclose(fp); return NULL; }
    study_frc_reader_t *r = (study_frc_reader_t *)calloc(1, sizeof(*r));
    if (!r) { opus_decoder_destroy(dec); fclose(fp); return NULL; }
    r->fp = fp; r->dec = dec;
    r->num_frames = count_frames(fp);
    return r;
}
uint32_t study_frc_num_frames(const study_frc_reader_t *r) { return r->num_frames; }
uint32_t study_frc_rate(const study_frc_reader_t *r) { (void)r; return STUDY_CODEC_RATE; }

void study_frc_close(study_frc_reader_t *r) {
    if (!r) return;
    if (r->dec) opus_decoder_destroy(r->dec);
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
        if (plen <= 0 || plen > (int)sizeof(r->pkt)) return -1;
        if (fread(r->pkt, 1, (size_t)plen, r->fp) != (size_t)plen) return -1;
        int ns = opus_decode(r->dec, r->pkt, plen, r->pending, 960, 0);
        if (ns < 0) return -1;
        r->pending_n = ns;
    }
    return got;
}

study_frc_writer_t *study_frc_create(const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return NULL;
    int err = 0;
    OpusEncoder *enc = opus_encoder_create(STUDY_CODEC_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !enc) { fclose(fp); return NULL; }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(20000));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(4));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    study_frc_writer_t *w = (study_frc_writer_t *)calloc(1, sizeof(*w));
    if (!w) { opus_encoder_destroy(enc); fclose(fp); return NULL; }
    w->fp = fp; w->enc = enc;
    return w;
}

int study_frc_enc_frame(study_frc_writer_t *w, const int16_t *pcm, int samples) {
    if (!w || samples <= 0) return -1;
    int n = opus_encode(w->enc, pcm, samples, w->pkt, (opus_int32)sizeof(w->pkt));
    if (n < 0) return -1;
    uint8_t hb[2]; hb[0] = (uint8_t)(n & 0xFF); hb[1] = (uint8_t)((n >> 8) & 0xFF);
    if (fwrite(hb, 1, 2, w->fp) != 2) return -1;
    if (fwrite(w->pkt, 1, (size_t)n, w->fp) != (size_t)n) return -1;
    w->num_frames++;
    return 0;
}
uint32_t study_frc_written_frames(const study_frc_writer_t *w) { return w ? w->num_frames : 0; }

int study_frc_finalize(study_frc_writer_t *w) {
    if (!w) return -1;
    if (w->fp) { fclose(w->fp); w->fp = NULL; }
    if (w->enc) { opus_encoder_destroy(w->enc); w->enc = NULL; }
    free(w);
    return 0;
}
void study_frc_abort(study_frc_writer_t *w) {
    if (!w) return;
    if (w->fp) { fclose(w->fp); w->fp = NULL; }
    if (w->enc) { opus_encoder_destroy(w->enc); w->enc = NULL; }
    free(w);
}
