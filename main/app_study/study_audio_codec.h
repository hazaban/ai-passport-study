/*
 * study_audio_codec.h — 16kHz/单声道 Opus 裸流编解码（与语音包/钥匙扣同一格式）
 *
 * 文件格式：每包 = u16LE(包长) + Opus 帧(20ms)，无容器头。采样率恒 16000。
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define STUDY_CODEC_RATE     16000
#define STUDY_FRAME_SAMPLES  320      /* 20ms @16k */

typedef struct study_frc_reader study_frc_reader_t;

/* 打开并预扫描帧数；失败返回 NULL */
study_frc_reader_t *study_frc_open(const char *path);
uint32_t study_frc_num_frames(const study_frc_reader_t *r);   /* 扫描所得 */
uint32_t study_frc_rate(const study_frc_reader_t *r);         /* 恒 16000 */
int      study_frc_read_pcm(study_frc_reader_t *r, int16_t *out, int max_samples);
void     study_frc_close(study_frc_reader_t *r);

typedef struct study_frc_writer study_frc_writer_t;

/* 新建并写首包前的空文件（不写任何头） */
study_frc_writer_t *study_frc_create(const char *path);
int  study_frc_enc_frame(study_frc_writer_t *w, const int16_t *pcm, int samples);
int  study_frc_finalize(study_frc_writer_t *w);   /* 关文件；0 成功 */
void study_frc_abort(study_frc_writer_t *w);      /* 关文件（删除由调用方负责） */
uint32_t study_frc_written_frames(const study_frc_writer_t *w);
