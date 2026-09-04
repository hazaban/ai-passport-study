/* Host 测试片段 stub：仅为了让 study_audio_codec.c 在 STUDY_REC_OPUS=1 时能编译。
 * 运行时走 Speex 路径，不链接 opus 库，故这些声明不会真正被引用/解析。 */
#pragma once
#define OPUS_OK 0
#define OPUS_APPLICATION_VOIP 2048
#define OPUS_BANDWIDTH_NARROWBAND 1101
#define OPUS_SET_COMPLEXITY(x) 4002
typedef short opus_int16;
extern void *opus_decoder_create(int Fs, int ch, int *error);
extern void   opus_decoder_destroy(void *dec);
extern int    opus_decode(void *dec, const unsigned char *data, int len,
                          opus_int16 *pcm, int frame_size, int decode_fec);
extern void *opus_encoder_create(int Fs, int ch, int application, int *error);
extern void   opus_encoder_destroy(void *enc);
extern int    opus_encode(void *enc, const opus_int16 *pcm, int frame_size,
                          unsigned char *data, int max_data_bytes);
extern int    opus_encoder_ctl(void *enc, int request, ...);