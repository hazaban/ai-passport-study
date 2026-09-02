/*
 * study_task.h — 任务数据模型 + 存储抽象层 + CRUD 接口
 *
 * 存储设计思路：
 *   业务逻辑不直接依赖 NVS，而是通过 study_task_store_t vtable。
 *   - 单元测试：注入内存模拟实现（见 tests/test_study_task.c）
 *   - ESP32 固件：study_task_nvs.c 提供 NVS 实现
 *   这样所有业务规则（类别校验、排序、repeat 过滤、统计）在宿主可测，
 *   移植到其他平台只需重写 store vtable。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "study_category.h"

/* ---------- 任务对象 ---------- */

/* 时间用 hour/minute 分开，不设时间时填 -1（节省做 time_t 时区转换） */
#define TASK_TITLE_LEN  40
#define TASK_MAX_COUNT  256

typedef enum {
    STUDY_REPEAT_ONCE     = 0,   /* 单次，完成后次日不再显示 */
    STUDY_REPEAT_DAILY    = 1,   /* 每天重复 */
    STUDY_REPEAT_WEEKDAY  = 2,   /* 工作日（周一至周五） */
    STUDY_REPEAT_CUSTOM   = 3,   /* 自定义（bit0=周一..bit6=周日）*/
} study_repeat_t;

/* ---------- 子分类：在科目之下再细分（用户要求） ---------- */
#define STUDY_SUBTYPE_COUNT  7
typedef enum {
    SUBTYPE_GENERAL    = 0,  /* 通用（默认） */
    SUBTYPE_ZHENTI     = 1,  /* 历年真题 */
    SUBTYPE_DAILY_QUIZ = 2,  /* 每日一题 */
    SUBTYPE_CHAPTER    = 3,  /* 章节习题 */
    SUBTYPE_RECITE     = 4,  /* 背诵（英语单词/政治） */
    SUBTYPE_MOCK       = 5,  /* 模拟考试 */
    SUBTYPE_NOTES      = 6,  /* 笔记/知识点复习 */
} study_subtype_t;

typedef struct {
    int         id;          /* 0..STUDY_SUBTYPE_COUNT-1 */
    const char *name_cn;     /* 中文显示名 */
} study_subtype_info_t;

const study_subtype_info_t *study_subtype_get(int id);

typedef struct {
    int        id;                       /* 主键，正整数；add 成功后回填 */
    char       title[TASK_TITLE_LEN];    /* 任务标题 */
    int        category;                 /* 0..STUDY_CATEGORY_COUNT-1 */
    uint8_t    subtype;                  /* 0..STUDY_SUBTYPE_COUNT-1 (study_subtype_t) */
    int8_t     hour;                     /* 提醒时 0..23 或 -1 表示未设 */
    int8_t     minute;                   /* 提醒分 0..59 或 -1 表示未设 */
    uint8_t    repeat;                   /* study_repeat_t / 自定义 weekday mask */
    bool       done;                     /* 今日是否完成 */
    uint32_t   done_at;                  /* 完成时的 unix 时间戳（秒），未完成=0 */
    uint32_t   created_at;               /* 创建 unix 时间戳（秒） */
} study_task_t;

/* ---------- 每日统计 ---------- */
typedef struct {
    int total;                                         /* 今日任务总数 */
    int done;                                          /* 已完成数 */
    int per_category[STUDY_CATEGORY_COUNT];            /* 每科已完成数 */
} study_daily_stats_t;

/* ---------- 预设常用任务（用于「快速添加模板」） ---------- */
typedef struct {
    const char *title;
    int         category;
    uint8_t     subtype;   /* study_subtype_t */
    int8_t      hour;      /* -1 表示不预设时间 */
    int8_t      minute;
} study_preset_t;

/* ---------- 存储抽象层 vtable ---------- */
typedef struct {
    int  (*save_task)(const study_task_t *t);          /* 新增或覆盖；0=成功 */
    int  (*load_task)(int id, study_task_t *out);      /* 0=成功 */
    int  (*delete_task)(int id);                       /* 0=成功 */
    int  (*all_task_ids)(int *out_ids, int max, int *out_count);
    int  (*next_id)(void);                             /* 分配唯一 ID */
    int  (*load_meta_int)(const char *k, int def);
    int  (*save_meta_int)(const char *k, int v);
} study_task_store_t;

/* 在使用任何 CRUD API 之前必须先设置存储层 */
void study_task_set_store(const study_task_store_t *store);

/* ---------- CRUD ---------- */

/* 新建任务。成功返回新 ID（>0），失败返回 -1。
 * 会：校验 category、title 非空、时间范围合法；回填 t->id 和 t->created_at。
 */
int  study_task_add(study_task_t *t);

int  study_task_get(int id, study_task_t *out);
int  study_task_delete(int id);

/* 标记完成/取消；会同时写入/清除 done_at 时间戳 */
int  study_task_mark_done(int id, bool done);

/* ---------- 查询 ---------- */

/* 列出今日应该显示的任务 ID，按"有时间的升序在前，无时间的在后"排序。
 * 返回实际数量；缓冲区不够时截断到 max。
 * 过滤规则：
 *   - STUDY_REPEAT_ONCE 且 done=false → 显示
 *   - STUDY_REPEAT_ONCE 且 done=true  → **不显示**（已了结的一次性任务）
 *   - STUDY_REPEAT_DAILY / WEEKDAY / CUSTOM → 永远显示（按 weekday 匹配，但
 *     纯内存测试不具备真实星期判断能力，简化为全部显示，ESP32 端会按
 *     localtime 做精确过滤）
 */
int  study_task_list_today(int *out_ids, int max);

/* 基于 list_today 计算今日统计 */
void study_task_compute_today_stats(study_daily_stats_t *out);

/* 清理所有 STUDY_REPEAT_ONCE 且 done=true 的任务。
 * 返回实际清理数；可被「清除已完成」按钮或每日 0 点刷新调用 */
int  study_task_archive_done_once(void);

/* 获取预设模板列表；返回数量，*presets 指向静态数组 */
int  study_task_presets(const study_preset_t **presets);
