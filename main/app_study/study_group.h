/*
 * study_group.h — 分组视图（左页/右页 + 日常秩序/各科目 双分区）
 *
 * UI 布局 (按用户要求)：
 *   左右两个 Tab（虚拟）：
 *     左 Tab = PENDING：未完成任务
 *        内部分两大组：
 *          ├ GROUP 1: 日常秩序 (category == CAT_DAILY)
 *          └ GROUP 2: 各科目学习 (category != CAT_DAILY)
 *
 *     右 Tab = DONE：已完成任务
 *        同样分日常秩序 / 各科目 两组
 */
#pragma once

#include <stdint.h>
#include "study_task.h"

typedef enum {
    STUDY_GROUP_DAILY_ORDER = 0,   /* 日常秩序类 (CAT_DAILY) */
    STUDY_GROUP_SUBJECTS    = 1,   /* 各科目学习 (!= CAT_DAILY) */
    STUDY_GROUP_ALL         = 2,   /* 全部（用于总统计） */
} study_group_t;

typedef enum {
    STUDY_DONE_PENDING = 0,   /* 未完成 */
    STUDY_DONE_DONE    = 1,   /* 已完成 */
} study_done_filter_t;

/* 列出指定分组 + 完成态筛选的今日任务 ID，
 * 按「时间升序在前、无时间在后」规则排序（和 list_today 一致）。
 * 返回实际数量；缓冲区不够时截断到 max。 */
int  study_group_list_today(study_group_t group,
                            study_done_filter_t done_filter,
                            int *out_ids, int max);

/* 快速统计某个 group 里 未完成 / 已完成 / 总数 的个数 */
typedef struct {
    int total;
    int pending;
    int done;
} study_group_count_t;

void study_group_count_today(study_group_t group, study_group_count_t *out);
