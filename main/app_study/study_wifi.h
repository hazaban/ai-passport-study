/*
 * study_wifi.h — WiFi 连接 + 本地网页配网 (SoftAP + HTTP)
 *
 * 背景：设备只有 3 键无键盘，配网靠手机辅助"网页配网"最友好：
 *   1) 设备在没有已存凭证时自动开启 SoftAP 热点，或用户到设置页手动开启。
 *   2) 手机连接该热点，浏览器打开 http://192.168.4.1 填 WiFi 账号密码提交。
 *   3) 设备收到后存 NVS → 关闭 AP → 连接该 WiFi → SNTP 校时。
 *
 * 凭证存独立 NVS 命名空间 "wifi"，与任务("tasks"/"config")隔离。
 */
#pragma once

#include <stdbool.h>

typedef enum {
    WIFI_STATE_IDLE = 0,      /* 未连接、未配网 */
    WIFI_STATE_AP_CONFIG,     /* SoftAP 配网中(热点已开，等手机提交) */
    WIFI_STATE_CONNECTING,    /* 已拿到凭证，正在连接路由器 */
    WIFI_STATE_CONNECTED,     /* 已连接并拿到 IP */
    WIFI_STATE_FAILED,        /* 连接失败 */
} study_wifi_state_t;

typedef void (*study_wifi_cb_t)(study_wifi_state_t st, void *user);

/* NVS 中是否已保存过 WiFi 凭证 */
bool study_wifi_has_stored_creds(void);

/* 初始化 WiFi 环境；若已有凭证则自动连接 STA，否则进入 AP 配网等待。
 * ap_ssid_prefix：AP 热点名称前缀(长度<=20)，自动追加后 4 位 MAC。 */
void study_wifi_init(const char *ap_ssid_prefix);

/* 手动开启/重新开启网页配网(SoftAP+HTTP)。可从设置页调用。 */
void study_wifi_start_ap_config(void);

/* 主动断开 WiFi / 停止配网。 */
void study_wifi_stop(void);

/* 状态 / SSID / 信号(dBm, 未连接=0) */
study_wifi_state_t study_wifi_get_state(void);
const char        *study_wifi_get_ssid(void);
int                study_wifi_get_rssi(void);

/* 当前配网热点的名称 / 密码（供硬件屏幕直接显示，方便手机连接） */
const char        *study_wifi_get_ap_ssid(void);

/* 注册状态回调(单回调，从事件/任务上下文触发，UI 需经 lvgl lock)。 */
void study_wifi_set_callback(study_wifi_cb_t cb, void *user);

/* 配网热点默认网关地址(便于 UI 展示给用户) */
#define STUDY_WIFI_AP_GATEWAY  "192.168.4.1"
/* 配网热点默认端口 */
#define STUDY_WIFI_AP_PORT     80
/* 配网热点密码（手机连接热点时需输入，屏幕会显示） */
#define STUDY_WIFI_AP_PASS     "liyufan408"  /* WPA2 密码需 ≥8 位，否则热点无法开启 */