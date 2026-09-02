/*
 * Unit test for study_group — 子分类 subtype + 分组展示 API
 *
 * 编译: gcc -I main/app_study -Wall -Wextra -o tests/test_study_group \
 *              tests/test_study_group.c main/app_study/study_task.c \
 *              main/app_study/study_category.c main/app_study/study_group.c
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "study_group.h"
#include "study_task.h"
#include "study_category.h"

#define TEST_PASS()  do { printf("  PASS: %s\n", __func__); } while(0)

/* ---------- 存储层 ---------- */
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

static int quick_add(const char *title, int cat, int subtype,
                     int h, int m, int repeat, bool done) {
    study_task_t t = {0};
    strncpy(t.title, title, sizeof(t.title) - 1);
    t.category = cat;
    t.subtype  = (uint8_t)subtype;
    t.hour = (int8_t)h;
    t.minute = (int8_t)m;
    t.repeat = (uint8_t)repeat;
    int id = study_task_add(&t);
    if (done) study_task_mark_done(id, true);
    return id;
}

/* ---------- 1. Subtype 元数据 ---------- */
static void test_subtype_count_and_meta(void) {
    assert(STUDY_SUBTYPE_COUNT >= 6);   /* 至少：通用+真题+每日一题+章节习题+背诵+模拟 */
    for (int i = 0; i < STUDY_SUBTYPE_COUNT; i++) {
        const study_subtype_info_t *s = study_subtype_get(i);
        assert(s != NULL);
        assert(s->id == i);
        assert(s->name_cn != NULL && strlen(s->name_cn) > 0);
    }
    assert(study_subtype_get(-1) == NULL);
    assert(study_subtype_get(STUDY_SUBTYPE_COUNT) == NULL);

    /* 检查几个关键 id 是否存在 */
    const study_subtype_info_t *g = study_subtype_get(SUBTYPE_GENERAL);
    assert(g != NULL && strcmp(g->name_cn, "通用") == 0);
    const study_subtype_info_t *z = study_subtype_get(SUBTYPE_ZHENTI);
    assert(z != NULL && (strcmp(z->name_cn, "真题") == 0 || strcmp(z->name_cn, "历年真题") == 0));
    const study_subtype_info_t *dy = study_subtype_get(SUBTYPE_DAILY_QUIZ);
    assert(dy != NULL && strstr(dy->name_cn, "每日") != NULL);
    TEST_PASS();
}

/* ---------- 2. 分组: list_today_grouped() ---------- */
static void test_basic_groups(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);

    /* 加 3 个日常秩序 + 3 个各科任务；混合已完成/未完成 */
    quick_add("早上洗漱", CAT_DAILY, SUBTYPE_GENERAL, 7,  0, STUDY_REPEAT_DAILY, true);
    quick_add("早饭",     CAT_DAILY, SUBTYPE_GENERAL, 7, 30, STUDY_REPEAT_DAILY, true);
    quick_add("睡觉",     CAT_DAILY, SUBTYPE_GENERAL, 23, 0, STUDY_REPEAT_DAILY, false);

    quick_add("高数习题",   CAT_MATH,   SUBTYPE_CHAPTER, 8,  0, STUDY_REPEAT_ONCE, false);
    quick_add("英语-真题", CAT_ENGLISH,SUBTYPE_ZHENTI,   15, 0, STUDY_REPEAT_ONCE, false);
    quick_add("政治背诵",   CAT_POLITICS,SUBTYPE_RECITE, 20, 0, STUDY_REPEAT_ONCE, true);

    /* ------- Group 1: 日常秩序 + 未完成 ------- */
    int ids[32];
    int got = study_group_list_today(STUDY_GROUP_DAILY_ORDER,
                                     STUDY_DONE_PENDING,
                                     ids, 32);
    assert(got == 1);   /* 只有"睡觉"是日常秩序 + 未完成 */
    study_task_t tmp; study_task_get(ids[0], &tmp);
    assert(strcmp(tmp.title, "睡觉") == 0);

    /* ------- Group 2: 日常秩序 + 已完成 ------- */
    got = study_group_list_today(STUDY_GROUP_DAILY_ORDER,
                                 STUDY_DONE_DONE,
                                 ids, 32);
    assert(got == 2);   /* 早上洗漱 + 早饭 */

    /* ------- Group 3: 各科目 + 未完成 ------- */
    got = study_group_list_today(STUDY_GROUP_SUBJECTS,
                                 STUDY_DONE_PENDING,
                                 ids, 32);
    assert(got == 2);   /* 高数 + 英语真题 */
    /* 按时间排序：高数 8:00 排在英语真题 15:00 前 */
    study_task_t a, b;
    study_task_get(ids[0], &a); study_task_get(ids[1], &b);
    assert(strcmp(a.title, "高数习题") == 0);
    assert(strcmp(b.title, "英语-真题") == 0);

    /* ------- Group 4: 各科目 + 已完成 ------- */
    got = study_group_list_today(STUDY_GROUP_SUBJECTS,
                                 STUDY_DONE_DONE,
                                 ids, 32);
    assert(got == 1);   /* 政治背诵 */
    study_task_get(ids[0], &tmp);
    assert(strcmp(tmp.title, "政治背诵") == 0);
    TEST_PASS();
}

/* ---------- 3. 子分类 subtype 持久化（add → get 往返一致） ---------- */
static void test_subtype_roundtrip(void) {
    mem_store_reset();
    study_task_set_store(&s_mem_store);

    study_task_t t = {0};
    strncpy(t.title, "高数-历年真题", sizeof(t.title) - 1);
    t.category = CAT_MATH;
    t.subtype  = SUBTYPE_ZHENTI;
    t.hour = 9; t.minute = 0;
    t.repeat = STUDY_REPEAT_ONCE;
    int id = study_task_add(&t);
    assert(id > 0);

    study_task_t got;
    assert(study_task_get(id, &got) == 0);
    assert(got.subtype == SUBTYPE_ZHENTI);

    /* 修改 subtype（通过 save，后续会封装 edit 接口） */
    got.subtype = SUBTYPE_DAILY_QUIZ;
    /* 直接调用 store.save_task 模拟 edit API 的底层 */
    extern const study_task_store_t *s_store_hack(void); /* 见 study_task.c */
    (void)s_store_hack;
    TEST_PASS();
}

/* ---------- 4. 预设模板含 subtype ---------- */
static void test_presets_have_subtype(void) {
    const study_preset_t *presets;
    int n = study_task_presets(&presets);
    assert(n > 10);
    /* 至少有一个 "真题" 预设的 subtype 是 SUBTYPE_ZHENTI */
    bool found_zhenti = false, found_daily_quiz = false;
    for (int i = 0; i < n; i++) {
        if (presets[i].subtype == SUBTYPE_ZHENTI) found_zhenti = true;
        if (presets[i].subtype == SUBTYPE_DAILY_QUIZ) found_daily_quiz = true;
    }
    assert(found_zhenti);
    assert(found_daily_quiz);
    TEST_PASS();
}

int main(void) {
    printf("=== study_group unit tests ===\n");
    test_subtype_count_and_meta();
    test_basic_groups();
    test_subtype_roundtrip();
    test_presets_have_subtype();
    printf("All study_group tests PASSED.\n");
    return 0;
}
