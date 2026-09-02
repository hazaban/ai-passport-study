/*
 * study_task.c — 任务业务规则 + CRUD（基于可替换存储层）
 */
#include "study_task.h"
#include <string.h>
#include <stdlib.h>

/* ---------- 时间戳（测试环境没有 <time.h> 问题，用 weak 符号兜底） ---------- */
#if __has_include(<sys/time.h>)
#  include <sys/time.h>
static uint32_t now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)tv.tv_sec;
}
#else
static uint32_t s_now_mock = 1700000000u;
static uint32_t now_sec(void) { return ++s_now_mock; }
#endif

/* ---------- Subtype 元数据（与 study_subtype_t enum 一一对应） ---------- */
static const study_subtype_info_t s_subtypes[STUDY_SUBTYPE_COUNT] = {
    { SUBTYPE_GENERAL,     "通用"        },
    { SUBTYPE_ZHENTI,      "历年真题"    },
    { SUBTYPE_DAILY_QUIZ,  "每日一题"    },
    { SUBTYPE_CHAPTER,     "章节习题"    },
    { SUBTYPE_RECITE,      "背诵"        },
    { SUBTYPE_MOCK,        "模拟考试"    },
    { SUBTYPE_NOTES,       "笔记复习"    },
};

const study_subtype_info_t *study_subtype_get(int id) {
    if (id < 0 || id >= STUDY_SUBTYPE_COUNT) return NULL;
    return &s_subtypes[id];
}

/* ---------- 存储指针 ---------- */
static const study_task_store_t *s_store = NULL;

void study_task_set_store(const study_task_store_t *store) {
    s_store = store;
}

static bool store_ready(void) { return s_store != NULL; }

/* ---------- 校验 ---------- */
static bool task_validate(const study_task_t *t) {
    if (!t) return false;
    /* 标题必须非空（跳过前导空白） */
    int i = 0;
    while (t->title[i] == ' ' || t->title[i] == '\t') i++;
    if (t->title[i] == '\0') return false;

    /* 类别必须合法 */
    if (t->category < 0 || t->category >= STUDY_CATEGORY_COUNT) return false;

    /* subtype 必须合法 */
    if (t->subtype >= STUDY_SUBTYPE_COUNT) return false;

    /* 时间范围：或者都 -1（未设），或者在合法范围 */
    if (t->hour == -1 && t->minute == -1) return true;
    if (t->hour < 0 || t->hour > 23) return false;
    if (t->minute < 0 || t->minute > 59) return false;
    return true;
}

/* ---------- CRUD ---------- */

int study_task_add(study_task_t *t) {
    if (!store_ready() || !t) return -1;
    if (!task_validate(t)) return -1;

    t->id = s_store->next_id();
    if (t->id <= 0) return -1;
    t->created_at = now_sec();
    t->done = false;
    t->done_at = 0;

    if (s_store->save_task(t) != 0) return -1;
    return t->id;
}

int study_task_get(int id, study_task_t *out) {
    if (!store_ready() || id <= 0 || !out) return -1;
    return s_store->load_task(id, out);
}

int study_task_delete(int id) {
    if (!store_ready() || id <= 0) return -1;
    /* 先确认存在，避免静默删除不存在 ID 时测试误判 */
    study_task_t tmp;
    if (s_store->load_task(id, &tmp) != 0) return -1;
    return s_store->delete_task(id);
}

int study_task_mark_done(int id, bool done) {
    if (!store_ready() || id <= 0) return -1;
    study_task_t t;
    if (s_store->load_task(id, &t) != 0) return -1;
    t.done = done;
    t.done_at = done ? now_sec() : 0;
    return s_store->save_task(&t);
}

/* ---------- 查询与排序 ---------- */

static bool should_show_today(const study_task_t *t) {
    switch (t->repeat) {
        case STUDY_REPEAT_ONCE:
            /* 单次任务：完成与否都保留在今日列表中，作为"今天做了这个"的记录。
             * 长期清理可以通过 UI 的「清除已完成」或 NVS 压缩策略做。 */
            return true;
        case STUDY_REPEAT_DAILY:
        case STUDY_REPEAT_WEEKDAY:
        case STUDY_REPEAT_CUSTOM:
        default:
            return true;
    }
}

/* 比较两个任务"今天列表中谁排在前面"：
 *   都有时间 → 按时间升序；
 *   一个有时间一个无 → 有时间在前；
 *   都无时间 → 按创建顺序（ID 升序）。
 */
static int task_cmp_today(const study_task_t *a, const study_task_t *b) {
    bool a_has = (a->hour >= 0 && a->minute >= 0);
    bool b_has = (b->hour >= 0 && b->minute >= 0);
    if (a_has && b_has) {
        int ka = a->hour * 60 + a->minute;
        int kb = b->hour * 60 + b->minute;
        if (ka != kb) return ka - kb;
        return a->id - b->id;
    }
    if (a_has && !b_has) return -1;
    if (!a_has && b_has) return 1;
    return a->id - b->id;
}

int study_task_list_today(int *out_ids, int max) {
    if (!store_ready() || !out_ids || max <= 0) return 0;

    /* 1) 收集全部 ID */
    int all_ids[TASK_MAX_COUNT];
    int n = 0;
    if (s_store->all_task_ids(all_ids, TASK_MAX_COUNT, &n) != 0) return 0;

    /* 2) 过滤 today 可见的，把 task 对象暂存起来便于排序 */
    study_task_t keep[TASK_MAX_COUNT];
    int kc = 0;
    for (int i = 0; i < n; i++) {
        study_task_t t;
        if (s_store->load_task(all_ids[i], &t) != 0) continue;
        if (should_show_today(&t)) {
            keep[kc++] = t;
        }
    }

    /* 3) 简单插入排序（kc≤256，无所谓） */
    for (int i = 1; i < kc; i++) {
        study_task_t key = keep[i];
        int j = i - 1;
        while (j >= 0 && task_cmp_today(&keep[j], &key) > 0) {
            keep[j + 1] = keep[j];
            j--;
        }
        keep[j + 1] = key;
    }

    /* 4) 截断到 max，输出 id */
    int out_n = kc < max ? kc : max;
    for (int i = 0; i < out_n; i++) out_ids[i] = keep[i].id;
    return out_n;
}

void study_task_compute_today_stats(study_daily_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    int ids[TASK_MAX_COUNT];
    int n = study_task_list_today(ids, TASK_MAX_COUNT);

    for (int i = 0; i < n; i++) {
        study_task_t t;
        if (study_task_get(ids[i], &t) != 0) continue;
        out->total++;
        if (t.done) {
            out->done++;
            if (t.category >= 0 && t.category < STUDY_CATEGORY_COUNT) {
                out->per_category[t.category]++;
            }
        }
    }
}

int study_task_archive_done_once(void) {
    if (!store_ready()) return 0;
    int all_ids[TASK_MAX_COUNT];
    int n = 0;
    if (s_store->all_task_ids(all_ids, TASK_MAX_COUNT, &n) != 0) return 0;

    int removed = 0;
    for (int i = 0; i < n; i++) {
        study_task_t t;
        if (s_store->load_task(all_ids[i], &t) != 0) continue;
        if (t.repeat == STUDY_REPEAT_ONCE && t.done) {
            if (s_store->delete_task(all_ids[i]) == 0) removed++;
        }
    }
    return removed;
}

/* ---------- 预设任务模板 ---------- */
static const study_preset_t s_presets[] = {
    /* 日常秩序 (CAT_DAILY) */
    { "早上洗漱",                 CAT_DAILY,    SUBTYPE_GENERAL,  7,  0  },
    { "早饭",                     CAT_DAILY,    SUBTYPE_GENERAL,  7,  30 },
    { "开始学习（放好手机）",      CAT_DAILY,    SUBTYPE_GENERAL,  8,  0  },
    { "午饭+背单词+午休30分钟",   CAT_DAILY,    SUBTYPE_RECITE,   12, 0  },
    { "晚饭（后散步）",           CAT_DAILY,    SUBTYPE_GENERAL,  18, 0  },
    { "睡前洗漱",                 CAT_DAILY,    SUBTYPE_GENERAL,  22, 30 },
    { "睡觉（回顾今日）",         CAT_DAILY,    SUBTYPE_GENERAL,  23, 0  },
    { "洗头发",                   CAT_DAILY,    SUBTYPE_GENERAL,  -1, -1 },
    /* 考研科目 — 混合 subtype，为用户推荐多种练习方式 */
    { "高等数学-每日一题",        CAT_MATH,     SUBTYPE_DAILY_QUIZ, 8,  0  },
    { "高等数学-真题",            CAT_MATH,     SUBTYPE_ZHENTI,     8,  30 },
    { "高等数学-章节习题",        CAT_MATH,     SUBTYPE_CHAPTER,    9,  0  },
    { "线代-矩阵运算专题",        CAT_LINEAR,   SUBTYPE_CHAPTER,    10, 0  },
    { "线代-真题演练",            CAT_LINEAR,   SUBTYPE_ZHENTI,     10, 45 },
    { "概率论-每日一题",          CAT_PROB,     SUBTYPE_DAILY_QUIZ, 14, 0  },
    { "概率论练习",               CAT_PROB,     SUBTYPE_CHAPTER,    14, 30 },
    { "数据结构-链表/树/图专题",  CAT_DS,       SUBTYPE_CHAPTER,    9,  0  },
    { "数据结构-真题",            CAT_DS,       SUBTYPE_ZHENTI,     11, 0  },
    { "计组-CPU/内存/总线",       CAT_CO,       SUBTYPE_CHAPTER,    11, 0  },
    { "操作系统-进程/内存管理",   CAT_OS,       SUBTYPE_CHAPTER,    15, 0  },
    { "计算机网络-七层模型",      CAT_NETWORK,  SUBTYPE_NOTES,      16, 0  },
    { "计网-真题",                CAT_NETWORK,  SUBTYPE_ZHENTI,     16, 45 },
    { "英语-背单词",              CAT_ENGLISH,  SUBTYPE_RECITE,     12, 30 },
    { "英语-阅读理解",            CAT_ENGLISH,  SUBTYPE_CHAPTER,    17, 0  },
    { "英语-真题阅读",            CAT_ENGLISH,  SUBTYPE_ZHENTI,     17, 45 },
    { "政治-马原/毛中特/史纲背诵", CAT_POLITICS, SUBTYPE_RECITE,     20, 0  },
    { "政治-每日一题",            CAT_POLITICS, SUBTYPE_DAILY_QUIZ, 20, 30 },
    { "政治-模考选择题",          CAT_POLITICS, SUBTYPE_MOCK,       21, 0  },
};

int study_task_presets(const study_preset_t **presets) {
    if (presets) *presets = s_presets;
    return (int)(sizeof(s_presets) / sizeof(s_presets[0]));
}
