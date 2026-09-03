/*
 * app_study.h — 考研助手模块对外入口（供 main.c DEMOS 数组注册）
 *
 * 完全遵循 demo.h 的 demo_entry_t 三函数契约，见 main/main.c DEMOS[].
 */
#pragma once

#include <stdbool.h>
#include "bsp_button.h"

void app_study_enter(void);   /* 构建屏幕 + 启动 FreeRTOS 调度/播放任务 */
void app_study_exit(void);    /* 释放资源 + 停止播放任务 + 定时器 */
void app_study_key(bsp_btn_t btn, bsp_btn_ev_t ev);  /* 按键分发到当前 page */
/* 封面页长按 OK 时请求完全退出回目录；main.c 轮询此标志 */
bool app_study_wants_exit(void);
