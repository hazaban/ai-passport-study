/*
 * study_scheduler.h — 考研助手提醒调度：纯逻辑层
 *
 * 设计原则：
 *   - 不依赖 FreeRTOS / LVGL / 任何硬件，可在 Linux 宿主测试
 *   - 实际硬件端：一个 FreeRTOS 任务每分钟 tick 一次，调用
 *     study_sched_find_next(now_h, now_m, &ev)，如果返回 true 且
 *     ev.overdue_minutes>=0 且距离<=阈值，就弹窗+响铃，然后调
 *     study_sched_ack_fired(ev.task_id) 防重复提醒
 *   - 每日 0 点（或 RTC 报警）调用 study_scheduler_new_day() 清空 fire 位图
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ---------- 事件 ---------- */
typedef enum {
    STUDY_SCHED_TASK_DUE = 0,  /* 到点提醒：任务设定的时间已到 */
} study_sched_kind_t;

typedef struct {
    study_sched_kind_t kind;
    int     task_id;            /* 触发的任务 ID */
    int8_t  due_hour;           /* 原定时间: hour */
    int8_t  due_minute;         /* 原定时间: minute */
    int     overdue_minutes;    /* 已经过期多少分钟（当前>=due 时>=0；未到点时为负，表示还有多少分钟） */
} study_sched_ev_t;

/* ---------- 日常秩序 · 场景钩子 ---------- */
typedef enum {
    STUDY_SCENE_MORNING_WASH = 0,   /* 勾选「早上洗漱」→ 播放开启新一天文案 */
    STUDY_SCENE_START_STUDY,        /* 勾选「早饭」→ 启动学习/手机放远提醒 */
    STUDY_SCENE_LUNCH,              /* 勾选「午饭」→ 背单词+午休30分钟提醒 */
    STUDY_SCENE_DINNER,             /* 勾选「晚饭」→ 散步提醒 */
    STUDY_SCENE_NIGHT_WASH,         /* 勾选「睡前洗漱」*/
    STUDY_SCENE_SLEEP,              /* 勾选「睡觉」→ 今日总结 + 深睡 */
    STUDY_SCENE_HAIR_WASH,          /* 洗头发周期性提醒触发 */
} study_sched_scene_t;

/* ---------- 生命周期 ---------- */

/* 清空今日 fire 记录（启动时 / 跨日时调用） */
void study_scheduler_reset(void);

/* 跨日：等价于 reset + 保留长期配置 */
void study_scheduler_new_day(void);

/* ---------- 调度查询 ---------- */

/* 根据当前时间 (now_h, now_m)，在今日任务中查找下一个"需要触发"的任务。
 *
 * 选择规则（按优先级）：
 *   1. 跳过已 ack_fired 的（今天已经响过铃的）
 *   2. 跳过 STUDY_REPEAT_ONCE 且已 done=true 的（已了结的一次性任务）
 *   3. 跳过无时间（hour==-1）的任务（没有触发时刻概念）
 *   4. 在剩余中找 due 最早的那个
 *
 * 返回值：找到就 true，ev 填好。找不到（今天全响过/全是无时间）返回 false。
 *
 * ev.overdue_minutes：
 *   = 0  正在当刻（now == due）
 *   < 0  还没到（例如 now=7:00，due=8:00 → 值为 -60）
 *   > 0  已过期（例如 now=8:05，due=8:00 → 值为 5）
 */
bool study_sched_find_next(int now_h, int now_m, study_sched_ev_t *ev);

/* 标记 task 今日已响铃（避免重复提醒） */
void study_sched_ack_fired(int task_id);

/* 计算 from → to 的分钟差（允许跨日，即 23:50→0:10 = 20 分钟） */
int  study_sched_minutes_until(int from_h, int from_m, int to_h, int to_m);

/* ---------- 场景钩子 ---------- */

/* 当一个 CAT_DAILY 的任务被勾选完成时，判断它是否触发了某个日常秩序场景。
 * 若触发：返回 true，*scene 设为对应场景；否则返回 false。
 *
 * 识别依据：按任务 title 中的中文关键词匹配（设计文档 §3.2 固定场景）。
 * ESP32 端如果希望更稳定，也可以给 study_task_t 加一个 scene_tag 字段，
 * 这里先用关键词匹配即可，简单且满足 Phase 1。
 */
bool study_sched_scene_after_done(int task_id, study_sched_scene_t *scene);

/* ---------- 洗头发：周期性提醒（间隔天数可在设置中调整，默认 7 天） ---------- */
#define STUDY_HAIR_INTERVAL_MIN      3    /* 最少每 3 天 */
#define STUDY_HAIR_INTERVAL_MAX      30   /* 最多每 30 天 */
#define STUDY_HAIR_INTERVAL_DEFAULT  7    /* 默认 7 天 */

/* 记录"刚完成了一次洗头"：把 last_day 记为当天，并清空"已提醒"标记。
 * 之后第 interval 天早晨会触发一次提醒。 */
void study_sched_hair_set_last_day(long last_day);

/* 当前洗头间隔天数（默认 7）；设置后下次时间立即按新间隔计算 */
int  study_sched_hair_interval_days(void);
void study_sched_hair_set_interval(int days);   /* clamp 到 [MIN, MAX] */

/* 当前是否已过洗头日且正处于早晨窗口内 → 需要语音提醒。
 * 每个洗头日后只提醒一次（s_hair_fired 置位，直到下次洗头才复位）。 */
bool study_sched_hair_should_remind(long today_day, int now_h, int now_m);
long study_sched_hair_next_epoch_day(void);
