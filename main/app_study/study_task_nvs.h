/*
 * study_task_nvs.h — 将 study_task_store_t vtable 接入 ESP-IDF NVS
 *
 * 用法：
 *   #include "study_task_nvs.h"
 *   study_task_nvs_init();            // 打开 NVS，失败返回非 0
 *   study_task_set_store(study_task_nvs_store());  // 把 vtable 注入到 study_task
 *
 * 存储结构（对应设计方案 §7.1）：
 *   namespace "tasks" :
 *      task_count (u32)           有效任务数量
 *      next_id    (u32)           下一个 ID
 *      task_<id>  (blob JSON)     单条任务（长度 < 508B ，NVS blob 上限 4KB 够用）
 *   namespace "config" :
 *      voice_pack (string)
 *      rec_wake_h / rec_wake_m / rec_sleep_h / rec_sleep_m (u8)  早起/睡眠记录时间戳
 *      wake_h / wake_m (u8)      起床闹钟时间（默认 7:00，设置中可调）
 *      hair_last_day (u16)     上次洗头日的 epoch 天数
 *      hair_interval (u8)      洗头间隔天数（3~30，默认 7，设置中可调）
 *      theme (u8)          0=夜间 1=白天
 *      volume (u8)
 */
#pragma once
#include "study_task.h"

/* 初始化 NVS 命名空间并填好 vtable。返回 0 OK */
int  study_task_nvs_init(void);

/* 取得静态 vtable 指针；study_task_set_store() 注入 */
const study_task_store_t *study_task_nvs_store(void);
