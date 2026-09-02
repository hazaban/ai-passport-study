/*
 * study_time.h — 时间源：优先 SNTP 真实时间，无同步时回落手动时间
 *
 * 设计：闹钟(7:00)与到点提醒需要真实墙钟时间。
 *   1) 优先通过 SNTP(esp_netif_sntp) 从 NTP 服务器校时，联网后自动同步。
 *   2) 未联网 / 未同步时返回 false，调用方(通常回落 NVS 里手动设置的时间)。
 * 该模块只在 ESP32 端编译；宿主编译走空实现。
 */
#pragma once

#include <stdbool.h>

/* 初始化 SNTP（非阻塞，后台自动校时）。可多次调用，内部幂等。 */
void study_time_init(void);

/* 是否已获取到有效真实时间（粗略判据：系统时间 > 2000-01-01） */
bool study_time_synced(void);

/* 取当前本地时区的时/分。调用前提：已满足 study_time_synced()==true。
 * 未同步时返回的是无效占位，调用方不应使用。 */
void study_time_get_now(int *hour, int *min);

/* 取"自 epoch 起的天序号"（用于跨天周期提醒，如每 7 天洗头发）。
 * 未同步时返回 -1，调用方不应使用。同步后 = time(NULL)/86400。 */
long study_time_get_epoch_day(void);