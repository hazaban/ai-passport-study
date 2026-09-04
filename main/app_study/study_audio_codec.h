/*
 * study_audio_codec.h — 录音编解码（8kHz/单声道），与语音包 Opus 链路隔离。
 *
 * 编译期由 STUDY_REC_OPUS 选择录音编码：
 *   1 = Opus 窄带（SILK-only, complexity=0 以绕开 ~120KB 信号分析重路径，目标 ~6kbps）
 *   0 = IMA-ADPCM（4KB/s 固定，超低内存）
 *
 * 文件格式（保持 .FRC 扩展名，对 study_recorder 透明，头部自描述编码器）：
 *   FRC2: magic"FRC2"(4) + frames(4) + codec_id(1) 后接帧流
 *         每帧 = u16LE(len) + payload(len)
 *         · codec_id==OPUS : payload = 一个 Opus 包（变长）
 *         · codec_id==ADPCM: payload 固定 84B = pred(2)+idx(1)+resv(1)+80B
 *   FRC1（老 ADPCM）：仅 magic"FRC1"(4)+frames(4)，无 codec_id，帧流同 ADPCM。
 *   每帧输入 160 个 int16 PCM 样本（8kHz × 20ms）。
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* 录音编码选择：1=Opus(窄带, ~6kbps, 时长 ~5x ADPCM)；0=IMA-ADPCM(4KB/s, 极省内存) */
#define STUDY_REC_OPUS  1

#define STUDY_CODEC_RATE     8000        /* 录音重设 ES8311 到 8kHz 单声道 */
#define STUDY_FRAME_SAMPLES  160         /* 20ms @8kHz (8000 × 0.02 = 160) */
/* 固定 payload 布局(ADPCM): 2(pred) + 1(idx) + 1(resv) + 80(ADPCM nibbles) = 84 */
#define STUDY_FRC_PAYLOAD_LEN 84

/* FRC 容器：头部自描述编码器 ID */
#define STUDY_CODEC_ADPCM   0
#define STUDY_CODEC_OPUS    1

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
