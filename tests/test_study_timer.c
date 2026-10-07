/*
 * Unit test for study_timer — 正/倒计时状态机 + 按天统计 + 跨天归档
 *
 * 编译: gcc -I main/app_study -Wall -Wextra -o tests/test_study_timer \
 *              tests/test_study_timer.c main/app_study/study_timer.c
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "study_timer.h"

#define TEST_PASS()  do { printf("  PASS: %s\n", __func__); } while(0)

/* ---------- 内存存储（模拟 NVS blob） ---------- */
static char s_blob[256];
static int  s_blob_len = -1;   /* -1 = 无数据 */

static int mem_load(const char *key, char *out, int max) {
    (void)key;
    if (s_blob_len < 0) return -1;
    if (s_blob_len >= max) return -1;
    memcpy(out, s_blob, (size_t)s_blob_len);
    return s_blob_len;
}
static int mem_save(const char *key, const char *data, int len) {
    (void)key;
    if (len >= (int)sizeof(s_blob)) return -1;
    memcpy(s_blob, data, (size_t)len);
    s_blob_len = len;
    return 0;
}
static study_timer_store_t s_store = { mem_load, mem_save };

/* ---------- fake 时间源 ---------- */
static long s_fake_day = 10000L;
static long fake_day(void) { return s_fake_day; }

static void setup(void) {
    s_blob_len = -1;
    s_fake_day = 10000L;
    study_timer_set_store(&s_store);
    study_timer_set_epoch_day_src(fake_day);
    study_timer_init();
}

/* 正计时：开始累计、暂停停走、继续、结束计入统计 */
static void test_up_stop_records(void) {
    study_timer_set_mode(STUDY_TIMER_UP);
    assert(study_timer_elapsed() == 0);

    study_timer_start();
    for (int i = 0; i < 125; i++) study_timer_tick();
    assert(study_timer_elapsed() == 125);
    assert(study_timer_today_up() == 0);      /* 结束前不计入 */

    /* 暂停：不再推进 */
    study_timer_pause();
    for (int i = 0; i < 10; i++) study_timer_tick();
    assert(study_timer_elapsed() == 125);

    /* 继续：接着累计 */
    assert(study_timer_toggle_pause() == true);
    for (int i = 0; i < 60; i++) study_timer_tick();
    assert(study_timer_elapsed() == 185);

    /* 结束：累计计入今天 */
    study_timer_stop();
    assert(study_timer_today_up() == 185);
    assert(study_timer_today_total() == 185);
    assert(study_timer_state() == STUDY_TIMER_IDLE);
    assert(study_timer_elapsed() == 0);       /* 复位 */
    TEST_PASS();
}

/* 倒计时：自然走完 → just_finished + 计入统计 + 复位 */
static void test_down_finish_records(void) {
    study_timer_set_mode(STUDY_TIMER_DOWN);
    study_timer_set_countdown(90);            /* 1分30秒 */
    assert(study_timer_remaining() == 90);

    study_timer_start();
    for (int i = 0; i < 89; i++) study_timer_tick();
    assert(study_timer_remaining() == 1);
    assert(!study_timer_just_finished());

    study_timer_tick();                       /* 到零 */
    assert(study_timer_just_finished() == true);
    assert(study_timer_state() == STUDY_TIMER_IDLE);
    assert(study_timer_today_down() == 90);   /* 完整走完计入 */
    assert(study_timer_today_total() == 90);
    assert(study_timer_remaining() == 90);    /* 复位为总时长 */
    TEST_PASS();
}

/* 倒计时：手动结束不计入统计 */
static void test_down_manual_stop_not_recorded(void) {
    study_timer_set_mode(STUDY_TIMER_DOWN);
    study_timer_set_countdown(3600);
    study_timer_start();
    for (int i = 0; i < 200; i++) study_timer_tick();
    study_timer_stop();
    assert(study_timer_today_down() == 0);
    assert(study_timer_today_total() == 0);
    assert(study_timer_remaining() == 3600);  /* 复位可再开始 */
    TEST_PASS();
}

/* 跨天归档：tick 中发现新的一天 → 今天清零、昨天顺移 */
static void test_roll_days(void) {
    study_timer_set_mode(STUDY_TIMER_UP);
    study_timer_start();
    for (int i = 0; i < 600; i++) study_timer_tick();   /* 今天 600s */
    study_timer_stop();
    assert(study_timer_today_up() == 600);

    /* 第二天 */
    s_fake_day = 10001L;
    bool rolled = study_timer_tick();
    assert(rolled == true);
    assert(study_timer_today_up() == 0);                 /* 新的一天从 0 开始 */
    assert(study_timer_day_total(1) == 600);             /* 昨天还在槽1 */
    assert(study_timer_stats()->today_epoch == 10001L);
    TEST_PASS();
}

/* 持久化往返：stop 后重新 init（重启场景）统计仍在 */
static void test_persist_restart(void) {
    study_timer_set_mode(STUDY_TIMER_UP);
    study_timer_start();
    for (int i = 0; i < 300; i++) study_timer_tick();
    study_timer_stop();
    assert(study_timer_today_up() == 300);

    /* 模拟重启：重新 init，从 blob 恢复 */
    study_timer_init();
    assert(study_timer_today_up() == 300);
    assert(study_timer_stats()->today_epoch == 10000L);
    TEST_PASS();
}

/* init 时发现存储里的"今天"已是昨天 → 自动归档 */
static void test_init_rolls_stale_today(void) {
    /* 先制造跨天数据并存盘 */
    study_timer_set_mode(STUDY_TIMER_UP);
    study_timer_start();
    for (int i = 0; i < 500; i++) study_timer_tick();
    study_timer_stop();
    assert(study_timer_today_up() == 500);

    /* 三天后开机 */
    s_fake_day = 10003L;
    study_timer_init();
    assert(study_timer_today_up() == 0);
    assert(study_timer_day_total(1) == 500);   /* 移到昨天 */
    assert(study_timer_stats()->today_epoch == 10003L);
    TEST_PASS();
}

/* 正计时 + 倒计时混合统计各自独立 */
static void test_mixed_accumulate(void) {
    study_timer_set_mode(STUDY_TIMER_UP);
    study_timer_start();
    for (int i = 0; i < 100; i++) study_timer_tick();
    study_timer_stop();

    study_timer_set_mode(STUDY_TIMER_DOWN);
    study_timer_set_countdown(60);
    study_timer_start();
    for (int i = 0; i < 60; i++) study_timer_tick();   /* 自然走完 */
    assert(study_timer_just_finished());

    assert(study_timer_today_up() == 100);
    assert(study_timer_today_down() == 60);
    assert(study_timer_today_total() == 160);
    TEST_PASS();
}

int main(void) {
    setup();    test_up_stop_records();
    setup();    test_down_finish_records();
    setup();    test_down_manual_stop_not_recorded();
    setup();    test_roll_days();
    setup();    test_persist_restart();
    setup();    test_init_rolls_stale_today();
    setup();    test_mixed_accumulate();
    printf("ALL TIMER TESTS PASSED\n");
    return 0;
}
