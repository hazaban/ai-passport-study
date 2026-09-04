/* Host 测试用 stub：替换 ESP-IDF esp_log.h，把日志打到 stderr */
#pragma once
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_EARLY_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define ESP_EARLY_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)