/*
 * study_timer_nvs.h — 计时器统计的 NVS blob 存储
 *
 * 与 study_task_nvs.c 共用 "config" 命名空间（避免多开一个 NVS 分区句柄），
 * 按 study_timer_set_store() 的 vtable 提供 load/save blob，持久化最近 7 天统计。
 * 仅 ESP_PLATFORM 下有效；宿主编译时返回 NULL。
 */
#pragma once
#include "study_timer.h"

/* 取得静态 timer store vtable（NULL=未初始化/宿主编译）；注入 study_timer_set_store() */
const study_timer_store_t *study_timer_nvs_store(void);
