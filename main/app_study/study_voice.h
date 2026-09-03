/*
 * study_voice.h — 语音+音效播放编排
 *
 * 播放链路（硬件端）：
 *   用户触发 → study_voice_play_complete(category_id)
 *                  1) 先播放该科的专属 RTTTL 旋律（方波 PCM → bsp_audio）
 *                  2) 接着尝试播 SPIFFS 里对应的 ADPCM / PCM 语音
 *                  3) 找不到语音文件时 → 屏幕显示 encouragement 文案兜底（由 UI 处理）
 *
 * 文件抽象：
 *   为了在"宿主单元测试/无硬件"场景能编译通过，音频输出使用回调函数
 *   study_voice_set_output() 注入。ESP32 端直接绑到 bsp_audio_write()，
 *   测试端写到 /dev/null 或捕获缓冲区。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* 采样率：与 ES8311 播放和 RTTTL 渲染统一。8 kHz 1ch 16bit（体积减半，语音清晰够用） */
#define VOICE_SAMPLE_RATE   8000

/* 柔和“叮咚”提示音（替代原较吵的科目旋律），完成/提醒/场景通用 */
#define STUDY_VOICE_CHIME_RTTTL "DingDong:d=8,o=5,b=100:g5,c6,p,g5,c6"

/* ---------- 输出回调 ---------- */
/* 接收一组 16-bit PCM 采样并写出。ESP32: 调用 bsp_audio_write(buf, n*2) */
typedef void (*study_voice_output_cb_t)(const int16_t *buf, int num_samples);

/* 设置播放时使用的输出回调和音量（0..100）。
 * audio_set_format_hint 若为非 NULL：每开始一段新的播放前调用一次，
 * 告知底层此时 bsp_audio_set_format(VOICE_SAMPLE_RATE, 16, 1) 等，
 * 让 ESP32 端可以按需切换格式。 */
void study_voice_set_output(study_voice_output_cb_t cb,
                            void (*format_hint)(void));
void study_voice_set_volume(int percent);

/* ---------- 高层 API：UI 只需要调这些 ---------- */

/* 1. 任务完成时播放：先 RTTTL 旋律 → 再语音（按 category+subtype 定制解析）。
 * 内部会跑在独立的 FreeRTOS 任务中，不阻塞按键回调。
 * 返回 0 表示已入队成功。若当前正在播放则取消老的再开始。
 * 若不关心 subtype，传 -1 即可（会用该科默认文案）。 */
int  study_voice_play_complete(int category_id);
int  study_voice_play_complete_with_subtype(int category_id, int subtype);

/* 2. 日常秩序场景：播放 start_study / lunch / sleep 等单条语音
 * 并在缺失时 fallback 到 DAILY 类的 RTTTL。key = "start_study", "sleep" ... */
int  study_voice_play_scene(const char *voice_key);

/* 3. 单播一段 RTTTL 字符串（用于提醒到点的提示音，不加语音） */
int  study_voice_play_rtttl(const char *rtttl);

/* 4. 取消任何正在进行的播放 */
void study_voice_stop(void);

/* ---------- 低层工具：SPIFFS 语音文件是否存在 ---------- */
/* ESP32 实现会查 SPIFFS；宿主实现恒返回 false，从而走 RTTTL fallback。 */
bool study_voice_file_exists(const char *voice_key);

/* ---------- 语音文件读取抽象（SPIFFS 注入） ---------- */
/* 为了让纯宿主单元测试也能编译，study_voice 内部不直接依赖 esp_spiffs；
 * 而是由 ESP32 端注入一组「按 voice_key 打开 WAV」的回调，见 study_voice_set_fs()。
 * WAV 规格固定为 16kHz / 16bit / mono（与 VOICE_SAMPLE_RATE 一致）。 */
typedef struct {
    /* open: 按 voice_key 打开音频；handle 为 0 表示打开失败（文件不存在） */
    int   (*open)(const char *voice_key);
    /* read: 读到最多 max 字节；返回实际读取字节数；0 表示 EOF */
    int   (*read)(int handle, void *buf, int max);
    /* close: 关闭 handle，handle 为 0 时调用方不会调用 close */
    void  (*close)(int handle);
} study_voice_fs_t;

/* 注入/取消语音文件读取回调。传 NULL 表示禁用（此后所有语音走 RTTTL fallback）。 */
void study_voice_set_fs(const study_voice_fs_t *fs);

/* 完整的语音 key → 类别 ID 映射表，供内部查找 */
typedef struct {
    int         category;       /* -1 表示场景语音 */
    int         subtype;        /* -1 = 默认(该类通用)；0..6 = 对应子分类(study_subtype_t) */
    const char *voice_key;      /* 如 "complete_math", "sleep", "english_recite" */
} study_voice_map_t;

extern const study_voice_map_t  study_voice_map[];
extern const int                study_voice_map_size;

/* 按 (category, subtype) 找最合适的语音 key：
 *   优先 category+subtype 匹配 → 没找到再退到 category 的默认 complete_*。
 *   没有任何映射返回 NULL。 */
const char *study_voice_resolve_key(int category, int subtype);
