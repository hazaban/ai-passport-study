/*
 * study_audio_codec.h — 8kHz/单声道 IMA-ADPCM 裸流编解码（录音专用，不影响语音包 Opus 链路）
 *
 * 文件格式（保持 .FRC 扩展名，对 study_recorder 透明）：
 *   每帧 = u16LE(len) + [pred(2) + idx(1) + reserved(1)] + ADPCM 压缩字节(80)
 *   其中 len 为 payload 长度（不含前导的 2 字节 u16LE(len)），固定 = 84 字节。
 *   每帧输入 160 个 int16 PCM 样本（8kHz × 20ms），压缩后 80 字节（4-bit ADPCM）。
 *
 * ⚠ 本文件只服务录音 / 回放链路；app_study.c 的语音播报链路使用自己内嵌的 OpusDecoder，
 *   与本 codec 完全隔离，互不影响。
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define STUDY_CODEC_RATE     8000        /* 录音重设 ES8311 到 8kHz 单声道 */
#define STUDY_FRAME_SAMPLES  160         /* 20ms @8kHz (8000 × 0.02 = 160)，帧长恒定，与历史 ms_len = fr*20 公式一致 */
/* 固定 payload 布局: 2(pred) + 1(idx) + 1(resv) + 80(ADPCM nibbles) = 84 */
#define STUDY_FRC_PAYLOAD_LEN 84

typedef struct study_frc_reader study_frc_reader_t;

/* 打开并预扫描帧数；失败返回 NULL */
study_frc_reader_t *study_frc_open(const char *path);
uint32_t study_frc_num_frames(const study_frc_reader_t *r);   /* 扫描所得 */
uint32_t study_frc_rate(const study_frc_reader_t *r);         /* 恒 STUDY_CODEC_RATE */
int      study_frc_read_pcm(study_frc_reader_t *r, int16_t *out, int max_samples);
void     study_frc_close(study_frc_reader_t *r);

typedef struct study_frc_writer study_frc_writer_t;

/* 新建并写首包前的空文件（不写任何头） */
study_frc_writer_t *study_frc_create(const char *path);
int  study_frc_enc_frame(study_frc_writer_t *w, const int16_t *pcm, int samples);
int  study_frc_finalize(study_frc_writer_t *w);   /* 关文件；0 成功 */
void study_frc_abort(study_frc_writer_t *w);      /* 关文件（删除由调用方负责） */
uint32_t study_frc_written_frames(const study_frc_writer_t *w);
