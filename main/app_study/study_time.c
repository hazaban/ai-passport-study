/*
 * study_time.c — 时间源实现（SNTP）
 * 见 study_time.h 的说明。
 */
#include "study_time.h"

#ifdef ESP_PLATFORM

#include "esp_log.h"
#include "esp_sntp.h"
#include "time.h"

static const char *TAG = "study_time";

void study_time_init(void) {
    /* true：从 1970 开始计时，未同步前 gettimeofday 落在 1970s，
     * 便于用"年份>2000"判断是否已同步。 */
    setenv("TZ", "CST-8", 1);   /* 中国标准时间(UTC+8)，东八区 */
    tzset();

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_setservername(1, "ntp.aliyun.com");
    sntp_setservername(2, "");
    sntp_init();
    ESP_LOGI(TAG, "SNTP 已启动（后台校时中）");
}

bool study_time_synced(void) {
    time_t now = time(NULL);
    return now >= 946684800L;   /* 2000-01-01 00:00:00 UTC */
}

void study_time_get_now(int *hour, int *min) {
    if (!hour || !min) return;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    *hour = tmv.tm_hour;
    *min  = tmv.tm_min;
}

long study_time_get_epoch_day(void) {
    time_t now = time(NULL);
    if (now < 946684800L) return -1L;   /* 未同步 */
    return (long)(now / 86400L);
}

int study_time_days_until(int month, int day) {
    time_t now = time(NULL);
    if (now < 946684800L) return -1;    /* 未同步 */
    struct tm tmv;
    localtime_r(&now, &tmv);
    int y = tmv.tm_year + 1900;

    struct tm target = {0};
    target.tm_year = y - 1900;
    target.tm_mon  = month - 1;
    target.tm_mday = day;
    target.tm_isdst = -1;
    time_t tt = mktime(&target);
    if (tt < now) {                     /* 今年已过，顺延到明年 */
        target.tm_year = y - 1899;
        tt = mktime(&target);
    }
    return (int)((tt - now) / 86400L);
}

#else  /* 宿主编译：STUB */

void study_time_init(void) {}
bool study_time_synced(void) { return false; }
void study_time_get_now(int *hour, int *min) {
    if (!hour || !min) return;
    *hour = 9;
    *min  = 0;
}
long study_time_get_epoch_day(void) { return -1L; }
int study_time_days_until(int month, int day) { return 107; } /* 宿主测试占位 */

#endif