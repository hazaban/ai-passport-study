/* Host 测试：验证录音 Speex 编解码还原度（不开板，先黑白盒验证 codec 逻辑）。
 * 合成一段语音（含静音间隙），走 FRC 写→读→解码，与原始 PCM 对比 SNR。 */
#include "study_audio_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int esp_get_free_heap_size(void) { return 1234560; }

/* --- opus stub 实现：运行走 Speex 路径，绝不调用；仅满足链接 --- */
#define OPUS_OK 0
typedef short opus_int16;
void *opus_decoder_create(int Fs, int ch, int *error) { (void)Fs; (void)ch; if (error) *error = OPUS_OK; return 0; }
void   opus_decoder_destroy(void *dec)   { (void)dec; }
int    opus_decode(void *dec, const unsigned char *d, int len, opus_int16 *p, int fs, int fec) { (void)dec;(void)d;(void)len;(void)p;(void)fs;(void)fec; return -1; }
void *opus_encoder_create(int Fs, int ch, int app, int *error) { (void)Fs; (void)ch; (void)app; if (error) *error = OPUS_OK; return 0; }
void   opus_encoder_destroy(void *enc)   { (void)enc; }
int    opus_encode(void *enc, const opus_int16 *p, int fs, unsigned char *d, int max) { (void)enc;(void)p;(void)fs;(void)d;(void)max; return -1; }
int    opus_encoder_ctl(void *enc, int request, ...) { (void)enc; (void)request; return OPUS_OK; }

static double seg_snr(const int16_t *a, const int16_t *b, int n) {
    double se = 0, sp = 0;
    for (int i = 0; i < n; i++) {
        double e = (double)a[i] - b[i];
        se += e * e;
        sp += (double)a[i] * a[i];
    }
    if (sp < 1e-6) sp = 1e-6;
    return 10.0 * log10(sp / se);   /* -inf 高都由大误差引起 */
}

int main(void) {
    const int RATE = STUDY_CODEC_RATE;
    const int N = RATE;                 /* 1 秒语音 */
    int16_t *src = malloc(N * 2);
    int16_t *dur = malloc(N * 2);
    if (!src || !dur) { printf("OOM\n"); return 2; }

    /* 合成"语音"：220Hz 基频 + 440Hz 谐波，5Hz 浊音包络，间歇静音（模拟停顿） */
    for (int i = 0; i < N; i++) {
        double t = (double)i / RATE;
        double env = (sin(2 * M_PI * 4.0 * t) + 1.0) * 0.5; /* 0..1 */
        double sig = 0.5 * sin(2 * M_PI * 220.0 * t) + 0.30 * sin(2 * M_PI * 440.0 * t)
                   + 0.18 * sin(2 * M_PI * 880.0 * t);
        if (env < 0.18) sig = 0.0;       /* 静音隙 */
        src[i] = (int16_t)(sig * env * 20000.0);
    }

    /* 编码 → .frc */
    study_frc_writer_t *w = study_frc_create("/tmp/rec_test.frc");
    if (!w) { printf("FILE-CREATE-FAIL\n"); return 2; }
    for (int base = 0; base < N; base += STUDY_FRAME_SAMPLES)
        if (study_frc_enc_frame(w, src + base, STUDY_FRAME_SAMPLES) < 0) {
            printf("ENC-FRAME-FAIL base=%d\n", base); study_frc_abort(w); return 2;
        }
    uint32_t wf = study_frc_written_frames(w);
    if (study_frc_finalize(w) < 0) { printf("FINALIZE-FAIL\n"); return 2; }

    /* 解码 ← .frc */
    study_frc_reader_t *r = study_frc_open("/tmp/rec_test.frc");
    if (!r) { printf("OPEN-FAIL\n"); return 2; }
    uint32_t nf = study_frc_num_frames(r);
    int16_t buf[STUDY_FRAME_SAMPLES];
    int outn = 0;
    while (outn < N) {
        int got = study_frc_read_pcm(r, buf, STUDY_FRAME_SAMPLES);
        if (got <= 0) break;
        for (int i = 0; i < got && outn < N; i++) dur[outn++] = buf[i];
    }
    study_frc_close(r);

    /* 统计 */
    printf("== RESULT ==\n");
    printf("written_frames=%u  scanned_frames=%u  decoded_samples=%d (expect %d)\n",
           wf, nf, outn, N);
    if (outn != N) { printf("FAIL: sample-count mismatch\n"); return 1; }

    /* 搜索最佳时移：排除 Speex 约 7 样本的算法延迟，得真还原度 */
    int bestlag = 0; double bestc = -9;
    for (int lag = 0; lag <= 40; lag++) {
        double sxy = 0, pa = 0, pb = 0;
        for (int i = lag; i < N; i++) {
            sxy += (double)dur[i] * src[i - lag];
            pa  += (double)dur[i] * dur[i];
            pb  += (double)src[i - lag] * src[i - lag];
        }
        double c = (pa > 0 && pb > 0) ? sxy / sqrt(pa * pb) : 0;
        if (c > bestc) { bestc = c; bestlag = lag; }
    }
    /* 用最优时移算全局 SNR */
    double se = 0, sp = 0;
    for (int i = bestlag; i < N; i++) {
        double e = (double)dur[i] - src[i - bestlag];
        se += e * e; sp += (double)src[i - bestlag] * src[i - bestlag];
    }
    double snr = (sp > 1e-6 && se > 1e-9) ? 10 * log10(sp / se) : 99;
    printf("best_shift=%d  corr=%.2f  shifted_SNR_dB=%.1f\n", bestlag, bestc, snr);

    /* 判定：以相关度为主（>0.9 即还原成立），SNR 为次级佐证。
     * Speex NB Q3 对窄带语音本身就有量化噪声，纯看 SNR 阈值会误报 "CODEC-BROKEN"。 */
    printf("== VERDICT ==\n");
    if (bestc >= 0.90 && snr > 5.0)
        printf("CODEC-OK（corr=%.2f>0.9 SNR=%.1fdB，解码还原成立；剩余问题应在固件采集/播放链路）\n",
               bestc, snr);
    else if (bestc >= 0.70)
        printf("CODEC-BORDERLINE（corr=%.2f 偏低但非噪声，需结合实机听感判断）\n", bestc);
    else
        printf("CODEC-BROKEN（corr=%.2f 极低，解码即噪声，codec 逻辑需排查）\n", bestc);
    return 0;
}