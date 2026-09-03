/*
 * Unit test for study_category — 10 考研任务类别定义
 * 编译: gcc -I main/app_study -Wall -Wextra -o tests/test_study_category tests/test_study_category.c main/app_study/study_category.c
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "study_category.h"

#define TEST_PASS()  do { printf("  PASS: %s\n", __func__); } while(0)

static void test_category_count(void) {
    /* 设计文档明确 10 个类别 */
    assert(STUDY_CATEGORY_COUNT == 10);
    TEST_PASS();
}

static void test_category_ids_range(void) {
    /* ID 必须 0..COUNT-1 */
    for (int i = 0; i < STUDY_CATEGORY_COUNT; i++) {
        const study_category_t *cat = study_category_get(i);
        assert(cat != NULL);
        assert(cat->id == i);
    }
    /* 越界返回 NULL */
    assert(study_category_get(-1) == NULL);
    assert(study_category_get(STUDY_CATEGORY_COUNT) == NULL);
    assert(study_category_get(999) == NULL);
    TEST_PASS();
}

static void test_category_names_not_empty(void) {
    for (int i = 0; i < STUDY_CATEGORY_COUNT; i++) {
        const study_category_t *cat = study_category_get(i);
        assert(cat != NULL);
        assert(cat->name != NULL && strlen(cat->name) > 0);
        assert(cat->name_cn != NULL && strlen(cat->name_cn) > 0);
    }
    TEST_PASS();
}

static void test_category_colors_match_design(void) {
    /* 验证设计文档里的主题色 */
    assert(study_category_get(0)->color_hex == 0x82BE2D);   /* 日常秩序 草绿 */
    assert(study_category_get(1)->color_hex == 0xE43B2F);   /* 高数 红 */
    assert(study_category_get(2)->color_hex == 0xFFB23E);   /* 线代 橙 */
    assert(study_category_get(3)->color_hex == 0xFFD928);   /* 概率论 黄 */
    assert(study_category_get(4)->color_hex == 0x1689E8);   /* 数据结构 蓝 */
    assert(study_category_get(5)->color_hex == 0x9B59B6);   /* 计组 紫 */
    assert(study_category_get(6)->color_hex == 0x8B4513);   /* 操作系统 棕 */
    assert(study_category_get(7)->color_hex == 0x1ABC9C);   /* 计网 青 */
    assert(study_category_get(8)->color_hex == 0xFF6B9D);   /* 英语 粉 */
    assert(study_category_get(9)->color_hex == 0xC0392B);   /* 政治 深红 */
    TEST_PASS();
}

static void test_category_encouragement_not_empty(void) {
    for (int i = 0; i < STUDY_CATEGORY_COUNT; i++) {
        const study_category_t *cat = study_category_get(i);
        assert(cat->encouragement != NULL);
        assert(strlen(cat->encouragement) > 5); /* 至少有一句完整的话 */
    }
    TEST_PASS();
}

static void test_category_specific_encouragements(void) {
    /* 抽查几个类别的文案关键词（与最新定制文案一致） */
    const study_category_t *math = study_category_get(1);
    assert(strstr(math->encouragement, "洛必达") != NULL ||
           strstr(math->encouragement, "泰勒") != NULL ||
           strstr(math->encouragement, "拉格朗日") != NULL);

    const study_category_t *daily = study_category_get(0);
    assert(strstr(daily->encouragement, "手机") != NULL ||  /* 提醒不要玩手机 */
           strstr(daily->encouragement, "专注") != NULL);

    const study_category_t *ds = study_category_get(4);
    assert(strstr(ds->encouragement, "链表") != NULL ||
           strstr(ds->encouragement, "算法") != NULL);

    const study_category_t *english = study_category_get(8);
    assert(strstr(english->encouragement, "单词") != NULL ||
           strstr(english->encouragement, "阅读") != NULL);
    TEST_PASS();
}

static void test_category_rtttl_not_empty(void) {
    for (int i = 0; i < STUDY_CATEGORY_COUNT; i++) {
        const study_category_t *cat = study_category_get(i);
        assert(cat->rtttl != NULL);
        assert(strlen(cat->rtttl) > 10); /* RTTTL 最小也得有几个音符 */
        /* RTTTL 格式必须是 name:defaults:notes */
        int colons = 0;
        for (const char *p = cat->rtttl; *p; p++) {
            if (*p == ':') colons++;
        }
        assert(colons == 2);
    }
    TEST_PASS();
}

static void test_category_find_by_id(void) {
    const study_category_t *cat;
    cat = study_category_find_by_name_cn("日常秩序");
    assert(cat != NULL && cat->id == 0);

    cat = study_category_find_by_name_cn("高等数学");
    assert(cat != NULL && cat->id == 1);

    cat = study_category_find_by_name_cn("数据结构");
    assert(cat != NULL && cat->id == 4);

    cat = study_category_find_by_name_cn("不存在的类别");
    assert(cat == NULL);
    TEST_PASS();
}

int main(void) {
    printf("=== study_category unit tests ===\n");
    test_category_count();
    test_category_ids_range();
    test_category_names_not_empty();
    test_category_colors_match_design();
    test_category_encouragement_not_empty();
    test_category_specific_encouragements();
    test_category_rtttl_not_empty();
    test_category_find_by_id();
    printf("All %s tests PASSED.\n", "study_category");
    return 0;
}
