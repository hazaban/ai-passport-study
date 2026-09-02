/*
 * study_category.h — 考研助手 10 大任务类别定义
 * 纯数据模块：可在 ESP32 固件和 Linux 单元测试中复用
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define STUDY_CATEGORY_COUNT  10

typedef struct {
    int         id;           /* 0..STUDY_CATEGORY_COUNT-1 */
    const char *name;         /* 英文短名，用于调试/存储 */
    const char *name_cn;      /* 中文显示名，UI 用 */
    uint32_t    color_hex;    /* 主题色 RGB hex */
    const char *encouragement;/* 完成时的鼓励文案（水豚噜噜语气） */
    const char *rtttl;        /* 专属 RTTTL 完成旋律 */
} study_category_t;

/* 按 ID 取类别；越界或非法返回 NULL */
const study_category_t *study_category_get(int id);

/* 按中文显示名查找；找不到返回 NULL */
const study_category_t *study_category_find_by_name_cn(const char *name_cn);

/* ---------- 类别 ID 常量，便于代码引用 ---------- */
#define CAT_DAILY        0   /* 日常秩序 */
#define CAT_MATH         1   /* 高等数学 */
#define CAT_LINEAR       2   /* 线性代数 */
#define CAT_PROB         3   /* 概率论 */
#define CAT_DS           4   /* 数据结构 */
#define CAT_CO           5   /* 计算机组成原理 */
#define CAT_OS           6   /* 操作系统 */
#define CAT_NETWORK      7   /* 计算机网络 */
#define CAT_ENGLISH      8   /* 英语 */
#define CAT_POLITICS     9   /* 政治 */
