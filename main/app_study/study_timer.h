/*
 * study_timer.h — 计时器核心逻辑（正计时 / 倒计时 / 按天统计）
 *
 * 与 UI 解耦：本模块只维护状态机与统计，不碰 LVGL/按键。
 * 存储抽象为 blob 注入（ESP32 用 NVS，宿主单元测试用内存），
 * 时间源抽象为 epoch-day 注入（ESP32 用 study_time，测试用 fake）。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ---------- 运行状态 ---------- */
typedef enum {
    STUDY_TIMER_IDLE   = 0,   /* 未开始 / 已结束复位 */
    STUDY_TIMER_RUNNING,      /* 运行中（计时推进） */
    STUDY_TIMER_PAUSED,       /* 暂停（暂停期间不计时） */
} study_timer_state_t;

/* ---------- 计时模式 ---------- */
typedef enum {
    STUDY_TIMER_UP   = 0,     /* 正计时：累计专注时长 */
    STUDY_TIMER_DOWN,         /* 倒计时：到零结束，完整走完才计入统计 */
} study_timer_mode_t;

/* ---------- 统计：7 天滚动槽，槽 0 = 今天 ---------- */
typedef struct {
    long     today_epoch;     /* 槽 0 对应的 epoch day；-1 表示从未有数据 */
    uint32_t up_sec[7];       /* 各天正计时秒 */
    uint32_t dn_sec[7];       /* 各天倒计时秒（完整走完的倒计时） */
} study_timer_stats_t;

/* ---------- 存储注入 ---------- */
typedef struct {
    /* 读取 blob：成功填 out 返回长度(≥0)；失败/不存在返回 -1 */
    int  (*load_blob)(const char *key, char *out, int max);
    /* 保存 blob：成功返回 0 */
    int  (*save_blob)(const char *key, const char *data, int len);
} study_timer_store_t;

/* 注入存储（ESP32 端在初始化时调用；宿主测试注入内存实现）。 */
void study_timer_set_store(const study_timer_store_t *s);

/* 注入"当前 epoch day"来源（默认 ESP 用 study_time；测试可注入 fake 模拟跨天）。
 * 返回 -1 表示时间未知（此时不做跨天归档，按内存态继续）。 */
void study_timer_set_epoch_day_src(long (*fn)(void));

/* 初始化：读入统计并处理"上次到今天是几天前"的归档。可在开机时调用一次。 */
void study_timer_init(void);

/* ---------- 操作 ---------- */
/* 切换模式：正/倒计时切换时复位为 IDLE（不清统计）。
 * 模式切换后需先 set_countdown 再 start（倒计时）。 */
void study_timer_set_mode(study_timer_mode_t m);
study_timer_mode_t study_timer_mode(void);

/* 设置倒计时总时长（秒，>0）。会复位为 IDLE、剩余=总时长。 */
void study_timer_set_countdown(int seconds);
int  study_timer_countdown_total(void);

void study_timer_start(void);        /* IDLE/PAUSED → RUNNING */
void study_timer_pause(void);        /* RUNNING → PAUSED */
bool study_timer_toggle_pause(void); /* 运行/暂停互切；IDLE 时等同 start。返回是否运行中 */
void study_timer_stop(void);         /* 结束：
                                     * 正计时 → 累计秒计入今天；
                                     * 倒计时 → 手动结束不计入统计。复位 IDLE。 */

study_timer_state_t study_timer_state(void);
bool study_timer_is_running(void);

/* 每秒推进一次（由应用层 tick 调用）。返回 true 表示发生了跨天归档。 */
bool study_timer_tick(void);

/* 倒计时是否刚在本 tick 内自然走完（应用层据此播提示音）。读取后自动清除。 */
bool study_timer_just_finished(void);

/* ---------- 读数 ---------- */
uint32_t study_timer_elapsed(void);    /* 正计时已累计秒 */
uint32_t study_timer_remaining(void);  /* 倒计时剩余秒 */

/* ---------- 统计查询 ---------- */
uint32_t study_timer_today_total(void);      /* 今天正+倒 总秒 */
uint32_t study_timer_today_up(void);         /* 今天正计时秒 */
uint32_t study_timer_today_down(void);       /* 今天倒计时秒 */
uint32_t study_timer_day_total(int offset);  /* offset 0=今天 1..6 */
const study_timer_stats_t *study_timer_stats(void);
