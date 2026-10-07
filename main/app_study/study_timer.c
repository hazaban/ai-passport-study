/*
 * study_timer.c — 计时器核心逻辑实现
 *
 * 设计约定：
 *   · 正计时：start 后每秒累计；stop 把累计秒计入当天 up_sec。
 *   · 倒计时：set_countdown 后 start；自然走完（到零）才把总时长计入当天
 *     dn_sec 并触发 just_finished（应用层播提示音）；手动 stop 不计入。
 *   · 暂停：暂停期间 tick 不推进。
 *   · 统计按 epoch-day 滚动归档，槽 0 = 今天，最多保留 7 天。
 *
 * 存储：单 key "tm_stats" blob，格式 "epoch,up0,dn0,...,up6,dn6"（秒）。
 * 时间源：默认使用 study_time（ESP），可通过 set_epoch_day_src 注入覆盖（测试）。
 */
#include "study_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "study_time.h"
#include <time.h>
#endif

#define STATS_KEY        "tm_stats"
#define STATS_SLOTS      7

static study_timer_state_t  s_state = STUDY_TIMER_IDLE;
static study_timer_mode_t   s_mode  = STUDY_TIMER_UP;
static uint32_t s_elapsed   = 0;   /* 正计时累计秒 */
static int      s_total     = 0;   /* 倒计时总秒 */
static uint32_t s_remaining = 0;   /* 倒计时剩余秒 */
static bool     s_just_finished = false;

static study_timer_stats_t s_stats;
static const study_timer_store_t *s_store = NULL;
static long (*s_epoch_src)(void) = NULL;

/* ---------- 时间源 ---------- */
static long timer_epoch_day(void) {
    if (s_epoch_src) return s_epoch_src();
#ifdef ESP_PLATFORM
    long d = study_time_get_epoch_day();
    if (d >= 0) return d;
    /* 未 SNTP 校时：用民用时间（含手动时间兜底）换算 epoch day */
    struct tm tm;
    if (study_time_civil_tm(&tm)) {
        time_t t = mktime(&tm);
        if (t > 0) return (long)(t / 86400L);
    }
#endif
    return -1L;
}

/* ---------- 统计序列化 ---------- */
static void stats_serialize(const study_timer_stats_t *st, char *out, int max) {
    int n = snprintf(out, max, "%ld", st->today_epoch);
    for (int i = 0; i < STATS_SLOTS && n < max - 1; i++) {
        n += snprintf(out + n, max - n, ",%u,%u", st->up_sec[i], st->dn_sec[i]);
    }
}

static int stats_parse(const char *s, study_timer_stats_t *st) {
    if (!s || !*s) return -1;
    const char *p = s;
    char *end = NULL;
    long e = strtol(p, &end, 10);
    if (end == p) return -1;
    st->today_epoch = e;
    p = end;
    for (int i = 0; i < STATS_SLOTS; i++) {
        if (*p != ',') return -1;
        p++;
        unsigned long v = strtoul(p, &end, 10);
        if (end == p) return -1;
        st->up_sec[i] = (uint32_t)v;
        p = end;
        if (*p != ',') return -1;
        p++;
        v = strtoul(p, &end, 10);
        if (end == p) return -1;
        st->dn_sec[i] = (uint32_t)v;
        p = end;
    }
    return 0;
}

static void save_stats(void) {
    if (!s_store || !s_store->save_blob) return;
    char buf[128];
    stats_serialize(&s_stats, buf, sizeof(buf));
    s_store->save_blob(STATS_KEY, buf, (int)strlen(buf));
}

static void load_stats(void) {
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.today_epoch = -1L;
    if (!s_store || !s_store->load_blob) return;
    char buf[128];
    int n = s_store->load_blob(STATS_KEY, buf, (int)sizeof(buf));
    if (n <= 0) return;
    buf[sizeof(buf) - 1] = 0;
    stats_parse(buf, &s_stats);
}

/* ---------- 跨天归档 ---------- */
static void roll_days(long new_day) {
    for (int i = STATS_SLOTS - 1; i > 0; i--) {
        s_stats.up_sec[i] = s_stats.up_sec[i - 1];
        s_stats.dn_sec[i] = s_stats.dn_sec[i - 1];
    }
    s_stats.up_sec[0] = 0;
    s_stats.dn_sec[0] = 0;
    s_stats.today_epoch = new_day;
    save_stats();
}

/* 检查是否需要归档；需要则归档并返回 true */
static bool maybe_roll(void) {
    long d = timer_epoch_day();
    if (d < 0) return false;
    if (s_stats.today_epoch == d) return false;
    roll_days(d);
    return true;
}

/* ---------- 公共 API ---------- */
void study_timer_set_store(const study_timer_store_t *s) { s_store = s; }
void study_timer_set_epoch_day_src(long (*fn)(void))      { s_epoch_src = fn; }

void study_timer_init(void) {
    load_stats();
    maybe_roll();
}

void study_timer_set_mode(study_timer_mode_t m) {
    s_mode = m;
    s_state = STUDY_TIMER_IDLE;
    s_elapsed = 0;
    s_remaining = 0;
    s_just_finished = false;
}
study_timer_mode_t study_timer_mode(void) { return s_mode; }

void study_timer_set_countdown(int seconds) {
    if (seconds < 1) seconds = 1;
    if (seconds > 24 * 3600) seconds = 24 * 3600;   /* 上限 24h，防误设 */
    s_total = seconds;
    s_remaining = (uint32_t)seconds;
    s_state = STUDY_TIMER_IDLE;
    s_just_finished = false;
}
int study_timer_countdown_total(void) { return s_total; }

void study_timer_start(void) {
    if (s_state == STUDY_TIMER_RUNNING) return;
    s_state = STUDY_TIMER_RUNNING;
}
void study_timer_pause(void) {
    if (s_state == STUDY_TIMER_RUNNING) s_state = STUDY_TIMER_PAUSED;
}
bool study_timer_toggle_pause(void) {
    if (s_state == STUDY_TIMER_RUNNING) { s_state = STUDY_TIMER_PAUSED; }
    else                                { s_state = STUDY_TIMER_RUNNING; }
    return s_state == STUDY_TIMER_RUNNING;
}

void study_timer_stop(void) {
    maybe_roll();   /* 结束时刻若已跨天先归档 */
    if (s_mode == STUDY_TIMER_UP) {
        s_stats.up_sec[0] += s_elapsed;
        save_stats();
    }
    /* 倒计时手动结束：不计入统计 */
    s_state = STUDY_TIMER_IDLE;
    s_elapsed = 0;
    s_remaining = (uint32_t)s_total;
    s_just_finished = false;
}

study_timer_state_t study_timer_state(void) { return s_state; }
bool study_timer_is_running(void) { return s_state == STUDY_TIMER_RUNNING; }

bool study_timer_tick(void) {
    bool rolled = maybe_roll();
    s_just_finished = false;
    if (s_state != STUDY_TIMER_RUNNING) return rolled;
    if (s_mode == STUDY_TIMER_UP) {
        s_elapsed++;
    } else {
        if (s_remaining > 0) s_remaining--;
        if (s_remaining == 0) {
            /* 倒计时自然走完：完整时长计入当天 */
            s_stats.dn_sec[0] += (uint32_t)s_total;
            save_stats();
            s_state = STUDY_TIMER_IDLE;
            s_remaining = (uint32_t)s_total;   /* 复位，便于再次开始 */
            s_just_finished = true;
        }
    }
    return rolled;
}

bool study_timer_just_finished(void) {
    bool r = s_just_finished;
    s_just_finished = false;
    return r;
}

uint32_t study_timer_elapsed(void)   { return s_elapsed; }
uint32_t study_timer_remaining(void) { return s_remaining; }

uint32_t study_timer_today_total(void)   { return s_stats.up_sec[0] + s_stats.dn_sec[0]; }
uint32_t study_timer_today_up(void)      { return s_stats.up_sec[0]; }
uint32_t study_timer_today_down(void)    { return s_stats.dn_sec[0]; }
uint32_t study_timer_day_total(int offset) {
    if (offset < 0) offset = 0;
    if (offset >= STATS_SLOTS) offset = STATS_SLOTS - 1;
    return s_stats.up_sec[offset] + s_stats.dn_sec[offset];
}
const study_timer_stats_t *study_timer_stats(void) { return &s_stats; }
