/*
 * Unit test for study_task — 任务 CRUD 与业务逻辑
 * 存储层使用可替换的 vtable（Linux 测试用内存模拟，ESP32 用 NVS）
 *
 * 编译: gcc -I main/app_study -Wall -Wextra -o tests/test_study_task \
 *              tests/test_study_task.c main/app_study/study_task.c main/app_study/study_category.c
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "study_task.h"
#include "study_category.h"

#define TEST_PASS()  do { printf("  PASS: %s\n", __func__); } while(0)

/* ---------- 内存模拟存储层 (测试专用) ---------- */
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
    .save_task       = mem_save_task,
    .load_task       = mem_load_task,
    .delete_task     = mem_delete_task,
    .all_task_ids    = mem_all_ids,
    .next_id         = mem_next_id,
    .load_meta_int   = mem_load_meta_int,
    .save_meta_int   = mem_save_meta_int,
};

/* ---------- 测试用例 ---------- */

static void test_add_task_basic(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);

    study_task_t t = {0};
    strncpy(t.title, "高数第三章习题", sizeof(t.title) - 1);
    t.category = CAT_MATH;
    t.hour = 8; t.minute = 0;
    t.repeat = STUDY_REPEAT_DAILY;

    int id = study_task_add(&t);
    assert(id > 0);              /* 新任务应该分配到正 ID */
    assert(t.id == id);          /* add 应该回填 id */
    assert(t.created_at > 0);    /* 必须打时间戳 */
    assert(t.done == false);     /* 初始未完成 */
    TEST_PASS();
}

static void test_add_task_id_increment(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);

    study_task_t t1 = {0};
    strncpy(t1.title, "A", sizeof(t1.title) - 1);
    int id1 = study_task_add(&t1);

    study_task_t t2 = {0};
    strncpy(t2.title, "B", sizeof(t2.title) - 1);
    int id2 = study_task_add(&t2);

    assert(id2 > id1);
    assert(id1 != id2);
    TEST_PASS();
}

static void test_add_task_invalid_category(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);

    study_task_t t = {0};
    strncpy(t.title, "bad", sizeof(t.title) - 1);
    t.category = 99;
    int id = study_task_add(&t);
    assert(id < 0);               /* 非法类别应拒绝 */
    TEST_PASS();
}

static void test_add_task_empty_title(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);

    study_task_t t = {0};
    /* title 全空 */
    int id = study_task_add(&t);
    assert(id < 0);
    TEST_PASS();
}

static void test_add_task_invalid_time(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);

    study_task_t t = {0};
    strncpy(t.title, "badtime", sizeof(t.title) - 1);
    t.category = CAT_MATH;
    t.hour = 25; t.minute = 70;   /* 非法时间 */
    int id = study_task_add(&t);
    assert(id < 0);

    /* 但允许不设时间 (hour=-1, minute=-1) */
    t.hour = -1; t.minute = -1;
    id = study_task_add(&t);
    assert(id > 0);
    TEST_PASS();
}

static void test_get_task(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);

    study_task_t t = {0};
    strncpy(t.title, "背单词50个", sizeof(t.title) - 1);
    t.category = CAT_ENGLISH;
    t.hour = 12; t.minute = 30;
    int id = study_task_add(&t);

    study_task_t got = {0};
    assert(study_task_get(id, &got) == 0);
    assert(strcmp(got.title, "背单词50个") == 0);
    assert(got.category == CAT_ENGLISH);
    assert(got.hour == 12 && got.minute == 30);

    /* 不存在的 ID 返回错误 */
    assert(study_task_get(9999, &got) != 0);
    TEST_PASS();
}

static void test_complete_task(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);

    study_task_t t = {0};
    strncpy(t.title, "链表复习", sizeof(t.title) - 1);
    t.category = CAT_DS;
    int id = study_task_add(&t);

    assert(t.done == false);
    int rc = study_task_mark_done(id, true);
    assert(rc == 0);

    study_task_t got;
    study_task_get(id, &got);
    assert(got.done == true);
    assert(got.done_at > 0);

    /* 取消完成 */
    rc = study_task_mark_done(id, false);
    assert(rc == 0);
    study_task_get(id, &got);
    assert(got.done == false);
    assert(got.done_at == 0);

    /* 对不存在的任务操作返回错误 */
    assert(study_task_mark_done(9999, true) != 0);
    TEST_PASS();
}

static void test_delete_task(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);

    study_task_t t = {0};
    strncpy(t.title, "要被删", sizeof(t.title) - 1);
    t.category = CAT_DAILY;
    int id = study_task_add(&t);

    assert(study_task_delete(id) == 0);
    study_task_t got;
    assert(study_task_get(id, &got) != 0);  /* 应该拿不到 */

    assert(study_task_delete(9999) != 0);   /* 重复删/不存在都返回错误 */
    TEST_PASS();
}

static void test_list_today_sorted_by_time(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);

    /* 故意乱序插入：10:30 DS,  无时间 Daily,  08:00 Math,  09:15 Linear */
    study_task_t t;

    memset(&t, 0, sizeof(t)); strncpy(t.title, "DS", sizeof(t.title)-1);
    t.category = CAT_DS; t.hour = 10; t.minute = 30;
    study_task_add(&t);

    memset(&t, 0, sizeof(t)); strncpy(t.title, "Wash", sizeof(t.title)-1);
    t.category = CAT_DAILY; t.hour = -1; t.minute = -1;  /* 无时间 */
    study_task_add(&t);

    memset(&t, 0, sizeof(t)); strncpy(t.title, "Math", sizeof(t.title)-1);
    t.category = CAT_MATH; t.hour = 8; t.minute = 0;
    study_task_add(&t);

    memset(&t, 0, sizeof(t)); strncpy(t.title, "Linear", sizeof(t.title)-1);
    t.category = CAT_LINEAR; t.hour = 9; t.minute = 15;
    study_task_add(&t);

    int ids[16];
    int n = study_task_list_today(ids, 16);
    assert(n == 4);

    /* 取出来看看顺序是否正确 */
    study_task_t arr[4];
    for (int i = 0; i < 4; i++) {
        study_task_get(ids[i], &arr[i]);
    }
    /* 按时间升序：08:00 Math, 09:15 Linear, 10:30 DS, 最后 Wash(无时间) */
    assert(strcmp(arr[0].title, "Math") == 0);
    assert(arr[0].hour == 8);
    assert(strcmp(arr[1].title, "Linear") == 0);
    assert(strcmp(arr[2].title, "DS") == 0);
    assert(strcmp(arr[3].title, "Wash") == 0);
    assert(arr[3].hour == -1);
    TEST_PASS();
}

static void test_list_today_respects_repeat(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);

    study_task_t t;

    /* 「单次 + 已完成」— 仍显示（灰显打勾，作为今日完成记录） */
    memset(&t, 0, sizeof(t));
    strncpy(t.title, "单次已完成", sizeof(t.title)-1);
    t.category = CAT_MATH;
    t.hour = 8; t.minute = 0;
    t.repeat = STUDY_REPEAT_ONCE;
    int id_done_once = study_task_add(&t);
    study_task_mark_done(id_done_once, true);

    /* 「单次 + 未完成」— 显示 */
    memset(&t, 0, sizeof(t));
    strncpy(t.title, "单次未完成", sizeof(t.title)-1);
    t.category = CAT_ENGLISH;
    t.hour = 9; t.minute = 0;
    t.repeat = STUDY_REPEAT_ONCE;
    study_task_add(&t);

    /* 「每日 + 已完成」— 显示（完成态保留） */
    memset(&t, 0, sizeof(t));
    strncpy(t.title, "每日已完成", sizeof(t.title)-1);
    t.category = CAT_DAILY;
    t.hour = 7; t.minute = 0;
    t.repeat = STUDY_REPEAT_DAILY;
    int id_done_daily = study_task_add(&t);
    study_task_mark_done(id_done_daily, true);

    (void)id_done_once; (void)id_done_daily;

    int ids[16];
    int n = study_task_list_today(ids, 16);
    /* 三个全部出现在今日列表里，按时间排序：每日(7:00) < 单次已完成(8:00) < 单次未完成(9:00) */
    assert(n == 3);

    study_task_t got;
    bool done_once_found = false;
    bool pending_once_found = false;
    bool done_daily_found = false;
    for (int i = 0; i < n; i++) {
        study_task_get(ids[i], &got);
        if (strcmp(got.title, "单次已完成") == 0) done_once_found = true;
        if (strcmp(got.title, "单次未完成") == 0) pending_once_found = true;
        if (strcmp(got.title, "每日已完成") == 0) done_daily_found = true;
    }
    assert(done_once_found);
    assert(pending_once_found);
    assert(done_daily_found);

    /* ---------- archive_done_once()：用户可清理「单次+已完成」的旧任务 ---------- */
    int archived = study_task_archive_done_once();
    assert(archived == 1);  /* 清掉那 1 个单次已完成 */

    n = study_task_list_today(ids, 16);
    assert(n == 2);   /* 只剩「单次未完成」和「每日已完成」 */
    bool found_pending = false, found_daily = false;
    for (int i = 0; i < n; i++) {
        study_task_get(ids[i], &got);
        if (strcmp(got.title, "单次未完成") == 0) found_pending = true;
        if (strcmp(got.title, "每日已完成") == 0) found_daily = true;
    }
    assert(found_pending);
    assert(found_daily);

    /* 再 archive 一次，应该没东西可清 */
    archived = study_task_archive_done_once();
    assert(archived == 0);

    TEST_PASS();
}

static void test_stats_basic(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);

    study_task_t t;
    memset(&t, 0, sizeof(t));
    strncpy(t.title, "A", sizeof(t.title)-1);
    t.category = CAT_MATH;
    int id1 = study_task_add(&t);

    memset(&t, 0, sizeof(t));
    strncpy(t.title, "B", sizeof(t.title)-1);
    t.category = CAT_DS;
    int id2 = study_task_add(&t);

    memset(&t, 0, sizeof(t));
    strncpy(t.title, "C", sizeof(t.title)-1);
    t.category = CAT_MATH;
    study_task_add(&t);

    /* 完成两个（A 和 B），C 不完成 */
    study_task_mark_done(id1, true);
    study_task_mark_done(id2, true);

    study_daily_stats_t s = {0};
    study_task_compute_today_stats(&s);

    assert(s.total == 3);
    assert(s.done  == 2);
    assert(s.per_category[CAT_MATH] == 1);   /* Math 完成1 */
    assert(s.per_category[CAT_DS]   == 1);   /* DS 完成1 */
    assert(s.per_category[CAT_ENGLISH] == 0);
    TEST_PASS();
}

static void test_presets_not_empty(void) {
    /* 预设常用任务模板列表 */
    const study_preset_t *presets;
    int n = study_task_presets(&presets);
    assert(n >= 10);
    for (int i = 0; i < n; i++) {
        assert(presets[i].title != NULL && strlen(presets[i].title) > 0);
        assert(presets[i].category >= 0 && presets[i].category < STUDY_CATEGORY_COUNT);
    }
    TEST_PASS();
}

int main(void) {
    printf("=== study_task unit tests ===\n");
    test_add_task_basic();
    test_add_task_id_increment();
    test_add_task_invalid_category();
    test_add_task_empty_title();
    test_add_task_invalid_time();
    test_get_task();
    test_complete_task();
    test_delete_task();
    test_list_today_sorted_by_time();
    test_list_today_respects_repeat();
    test_stats_basic();
    test_presets_not_empty();
    printf("All study_task tests PASSED.\n");
    return 0;
}
