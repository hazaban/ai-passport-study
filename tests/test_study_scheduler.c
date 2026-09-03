/*
 * Unit test for study_scheduler — 提醒调度的纯逻辑
 *
 * 编译: gcc -I main/app_study -Wall -Wextra -o tests/test_study_scheduler \
 *              tests/test_study_scheduler.c \
 *              main/app_study/study_scheduler.c \
 *              main/app_study/study_task.c \
 *              main/app_study/study_category.c
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "study_scheduler.h"
#include "study_task.h"
#include "study_category.h"

#define TEST_PASS()  do { printf("  PASS: %s\n", __func__); } while(0)

/* ---------- 模拟存储层（复用 test_study_task 的 mem 实现） ---------- */
#define MAX_STORE_TASKS  128
static study_task_t s_store[MAX_STORE_TASKS];
static int s_store_count;
static int s_store_next_id;

static int mem_save_task(const study_task_t *t) {
    if (s_store_count >= MAX_STORE_TASKS) return -1;
    for (int i = 0; i < s_store_count; i++) {
        if (s_store[i].id == t->id) { s_store[i] = *t; return 0; }
    }
    s_store[s_store_count++] = *t;
    return 0;
}
static int mem_load_task(int id, study_task_t *out) {
    for (int i = 0; i < s_store_count; i++) {
        if (s_store[i].id == id) { *out = s_store[i]; return 0; }
    }
    return -1;
}
static int mem_delete_task(int id) {
    for (int i = 0; i < s_store_count; i++) {
        if (s_store[i].id == id) {
            for (int j = i; j + 1 < s_store_count; j++) s_store[j] = s_store[j + 1];
            s_store_count--;
            return 0;
        }
    }
    return -1;
}
static int mem_all_ids(int *out_ids, int max, int *out_count) {
    int n = s_store_count < max ? s_store_count : max;
    for (int i = 0; i < n; i++) out_ids[i] = s_store[i].id;
    *out_count = s_store_count;
    return 0;
}
static int mem_next_id(void) { return ++s_store_next_id; }
static int mem_load_meta_int(const char *k, int def) { (void)k; return def; }
static int mem_save_meta_int(const char *k, int v) { (void)k; (void)v; return 0; }

static void mem_store_reset(void) {
    memset(s_store, 0, sizeof(s_store));
    s_store_count = 0;
    s_store_next_id = 0;
}

static study_task_store_t s_mem_store = {
    .save_task = mem_save_task, .load_task = mem_load_task,
    .delete_task = mem_delete_task, .all_task_ids = mem_all_ids,
    .next_id = mem_next_id,
    .load_meta_int = mem_load_meta_int, .save_meta_int = mem_save_meta_int,
};

/* ---------- 工具：快速加任务 ---------- */
static int quick_add(const char *title, int cat, int h, int m, int repeat) {
    study_task_t t = {0};
    strncpy(t.title, title, sizeof(t.title) - 1);
    t.category = cat;
    t.hour = (int8_t)h;
    t.minute = (int8_t)m;
    t.repeat = (uint8_t)repeat;
    return study_task_add(&t);
}

/* ---------- 测试 1: find_next_due 基础 ---------- */
static void test_find_next_no_tasks(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);
    study_scheduler_reset();

    study_sched_ev_t ev = {0};
    bool found = study_sched_find_next(7, 30, &ev);
    assert(found == false);  /* 没有任何任务 */
    TEST_PASS();
}

static void test_find_next_one_task(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);
    study_scheduler_reset();

    int id = quick_add("高数", CAT_MATH, 8, 0, STUDY_REPEAT_DAILY);

    /* 当前 7:00，下一个就是高数 8:00 */
    study_sched_ev_t ev = {0};
    bool found = study_sched_find_next(7, 0, &ev);
    assert(found);
    assert(ev.task_id == id);
    assert(ev.kind == STUDY_SCHED_TASK_DUE);
    assert(ev.due_hour == 8 && ev.due_minute == 0);

    /* 已经到 8:05，下一个仍然找到"未触发过"的 8:00 任务（表示过期触发） */
    found = study_sched_find_next(8, 5, &ev);
    assert(found);
    assert(ev.task_id == id);
    assert(ev.overdue_minutes == 5);   /* 过期 5 分钟 */
    TEST_PASS();
}

static void test_find_next_skips_already_fired(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);
    study_scheduler_reset();

    int id1 = quick_add("高数", CAT_MATH, 8, 0, STUDY_REPEAT_DAILY);
    int id2 = quick_add("线代", CAT_LINEAR, 9, 0, STUDY_REPEAT_DAILY);

    study_sched_ev_t ev = {0};
    /* 7:00 第一个应是 id1 */
    bool found = study_sched_find_next(7, 0, &ev);
    assert(found && ev.task_id == id1);

    /* 标记已触发（模拟定时器到点 fire 后调用 ack） */
    study_sched_ack_fired(id1);

    /* 再查下一个，应该是 id2 */
    found = study_sched_find_next(8, 1, &ev);
    assert(found);
    assert(ev.task_id == id2);
    (void)id1; (void)id2;
    TEST_PASS();
}

static void test_find_next_multiple_sorted(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);
    study_scheduler_reset();

    int id_ds    = quick_add("DS",    CAT_DS,       10, 30, STUDY_REPEAT_DAILY);
    int id_math  = quick_add("Math",  CAT_MATH,      8,  0, STUDY_REPEAT_DAILY);
    int id_lin   = quick_add("Lin",   CAT_LINEAR,    9, 15, STUDY_REPEAT_DAILY);
    /* 无时间的任务：不会被调度器触发（没有触发时间点） */
    quick_add("Wash", CAT_DAILY,      -1, -1, STUDY_REPEAT_DAILY);

    study_sched_ev_t ev = {0};
    bool found = study_sched_find_next(7, 0, &ev);
    assert(found);
    assert(ev.task_id == id_math);   /* 8:00 最早 */
    study_sched_ack_fired(id_math);

    found = study_sched_find_next(8, 0, &ev);
    assert(found && ev.task_id == id_lin);  /* 9:15 */
    study_sched_ack_fired(id_lin);

    found = study_sched_find_next(9, 30, &ev);
    assert(found && ev.task_id == id_ds);   /* 10:30 */
    study_sched_ack_fired(id_ds);

    /* 全触发完了，不再找到 */
    found = study_sched_find_next(11, 0, &ev);
    assert(found == false);
    TEST_PASS();
}

static void test_find_next_skips_done_once_only(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);
    study_scheduler_reset();

    /* 单次任务如果已完成，就不应该再触发提醒 */
    int id1 = quick_add("单次Math", CAT_MATH, 8, 0, STUDY_REPEAT_ONCE);
    study_task_mark_done(id1, true);

    /* 每日任务即使已完成，仍会触发到点提醒（防睡过） */
    int id2 = quick_add("每日English", CAT_ENGLISH, 9, 0, STUDY_REPEAT_DAILY);
    study_task_mark_done(id2, true);

    study_sched_ev_t ev = {0};
    bool found = study_sched_find_next(7, 0, &ev);
    assert(found);
    assert(ev.task_id == id2);   /* 只找到每日那个 */
    TEST_PASS();
}

/* ---------- 测试 2: minutes_until ---------- */
static void test_minutes_until_basic(void) {
    /* 8:00 → 10:30 = 150 min */
    assert(study_sched_minutes_until(8, 0, 10, 30) == 150);
    /* 同一时刻 = 0 */
    assert(study_sched_minutes_until(7, 7, 7, 7) == 0);
    /* 跨日：23:50 → 次日 0:10 = 20 min（scheduler 会用"明日"概念） */
    assert(study_sched_minutes_until(23, 50, 0, 10) == 20);
    TEST_PASS();
}

/* ---------- 测试 3: 日常秩序 · 场景钩子 ---------- */
static void test_hook_after_breakfast(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);
    study_scheduler_reset();

    /* 勾选「早饭」完成 */
    int bid = quick_add("早饭", CAT_DAILY, 7, 30, STUDY_REPEAT_DAILY);
    study_task_mark_done(bid, true);

    study_sched_scene_t scene;
    bool need = study_sched_scene_after_done(bid, &scene);
    assert(need == true);
    assert(scene == STUDY_SCENE_START_STUDY);   /* 早饭 → 开始学习提醒 */
    TEST_PASS();
}

static void test_hook_after_lunch(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);
    study_scheduler_reset();

    int lid = quick_add("午饭+背单词+午休30分钟", CAT_DAILY, 12, 0, STUDY_REPEAT_DAILY);
    study_task_mark_done(lid, true);

    study_sched_scene_t scene;
    bool need = study_sched_scene_after_done(lid, &scene);
    assert(need == true);
    assert(scene == STUDY_SCENE_LUNCH);
    TEST_PASS();
}

static void test_hook_after_morning_wash(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);
    study_scheduler_reset();

    int wid = quick_add("早上洗漱", CAT_DAILY, 7, 0, STUDY_REPEAT_DAILY);
    study_task_mark_done(wid, true);

    study_sched_scene_t scene;
    bool need = study_sched_scene_after_done(wid, &scene);
    assert(need == true);
    assert(scene == STUDY_SCENE_MORNING_WASH);
    TEST_PASS();
}

static void test_hook_sleep_button(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);
    study_scheduler_reset();

    int sid = quick_add("睡觉（回顾今日）", CAT_DAILY, 23, 0, STUDY_REPEAT_DAILY);
    study_task_mark_done(sid, true);

    study_sched_scene_t scene;
    bool need = study_sched_scene_after_done(sid, &scene);
    assert(need == true);
    assert(scene == STUDY_SCENE_SLEEP);
    TEST_PASS();
}

static void test_hook_not_daily_category(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);
    study_scheduler_reset();

    /* 完成高数任务：不会触发日常秩序场景 */
    int mid = quick_add("高数习题", CAT_MATH, 8, 0, STUDY_REPEAT_DAILY);
    study_task_mark_done(mid, true);

    study_sched_scene_t scene;
    bool need = study_sched_scene_after_done(mid, &scene);
    assert(need == false);   /* 不是日常秩序 → 无场景 */
    TEST_PASS();
}

/* ---------- 测试 4: 每日 0 点复位 fire 状态 ---------- */
static void test_reset_cross_midnight(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);
    study_scheduler_reset();

    quick_add("高数", CAT_MATH, 8, 0, STUDY_REPEAT_DAILY);
    study_sched_ev_t ev = {0};

    /* Day1：触发一次并 ack */
    bool found = study_sched_find_next(7, 0, &ev);
    assert(found);
    int fired_id = ev.task_id;
    study_sched_ack_fired(fired_id);

    found = study_sched_find_next(9, 0, &ev);
    assert(found == false);  /* 今天的已触发完 */

    /* 跨日：通知 scheduler 新的一天到来 */
    study_scheduler_new_day();

    /* Day2：8:00 的高数应该又能被找到（因为是每日任务） */
    found = study_sched_find_next(7, 0, &ev);
    assert(found);
    assert(ev.task_id == fired_id);
    TEST_PASS();
}

/* ---------- 测试 5: 洗头发 每 7 天早晨提醒 ---------- */
static void test_hair_not_due_yet(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);
    study_scheduler_reset();

    /* 今天（day=100）洗头，之后 7 天内都不该提醒 */
    study_sched_hair_set_last_day(100);

    for (int d = 100; d < 107; d++) {
        bool remind = study_sched_hair_should_remind(d, 8, 2);
        assert(remind == false);
    }
    TEST_PASS();
}

static void test_hair_due_at_morning_window(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);
    study_scheduler_reset();

    study_sched_hair_set_last_day(100);   /* 洗头日 */

    /* 第 7 天 = 107，早晨 8:00 窗口内 → 应提醒 */
    bool remind = study_sched_hair_should_remind(107, 8, 0);
    assert(remind == true);

    /* 同一周期第 2 次查询（仍在早晨）→ 已提醒过，不再响 */
    remind = study_sched_hair_should_remind(107, 8, 3);
    assert(remind == false);

    /* 错过早晨窗口（例如 9:00）→ 不再响（本周期只响一次） */
    study_sched_hair_set_last_day(200);
    remind = study_sched_hair_should_remind(207, 9, 0);
    assert(remind == false);

    TEST_PASS();
}

static void test_hair_reset_after_wash(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);
    study_scheduler_reset();

    /* 先在 107 天提醒过 */
    study_sched_hair_set_last_day(100);
    assert(study_sched_hair_should_remind(107, 8, 0) == true);

    /* 用户在 107 天又洗了头 → 重新计周期 */
    study_sched_hair_set_last_day(107);
    assert(study_sched_hair_should_remind(114, 8, 0) == true);   /* 又隔 7 天 */
    assert(study_sched_hair_should_remind(113, 8, 0) == false);  /* 没到 */
    TEST_PASS();
}

static void test_hair_never_washed(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);
    study_scheduler_reset();

    /* 从未洗过头：不应在任何时间提醒 */
    assert(study_sched_hair_should_remind(1, 8, 0) == false);
    assert(study_sched_hair_should_remind(999999, 8, 0) == false);
    TEST_PASS();
}

int main(void) {
    printf("=== study_scheduler unit tests ===\n");
    test_find_next_no_tasks();
    test_find_next_one_task();
    test_find_next_skips_already_fired();
    test_find_next_multiple_sorted();
    test_find_next_skips_done_once_only();
    test_minutes_until_basic();
    test_hook_after_breakfast();
    test_hook_after_lunch();
    test_hook_after_morning_wash();
    test_hook_sleep_button();
    test_hook_not_daily_category();
    test_reset_cross_midnight();
    test_hair_not_due_yet();
    test_hair_due_at_morning_window();
    test_hair_reset_after_wash();
    test_hair_never_washed();
    printf("All study_scheduler tests PASSED.\n");
    return 0;
}
