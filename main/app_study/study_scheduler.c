/*
 * study_scheduler.c — 调度纯逻辑实现
 */
#include "study_scheduler.h"
#include "study_task.h"
#include "study_category.h"
#include <string.h>
#include <stdlib.h>

/* fired 位图：按 task_id 的低 255 位记录今日是否已经响过铃。
 * task_id 是自增正整数，每天最多 256 个任务，完全够用。 */
#define SCHED_FIRED_BITS  256
static uint8_t s_fired[(SCHED_FIRED_BITS + 7) / 8];

static inline void fired_set(int id, bool v) {
    int bit = id & (SCHED_FIRED_BITS - 1);
    if (v) s_fired[bit >> 3] |=  (uint8_t)(1u << (bit & 7));
    else   s_fired[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
}
static inline bool fired_get(int id) {
    int bit = id & (SCHED_FIRED_BITS - 1);
    return (s_fired[bit >> 3] >> (bit & 7)) & 1u;
}

void study_scheduler_reset(void) {
    memset(s_fired, 0, sizeof(s_fired));
}

void study_scheduler_new_day(void) {
    study_scheduler_reset();
}

/* ---------- 洗头发：周期性提醒（间隔天数可配置） ---------- */
/* 早晨提醒窗口：8:00 ~ 8:05（每 30s tick 不会错过） */
#define HAIR_HOUR        8
#define HAIR_MINUTE      0
#define HAIR_WINDOW_MIN  5

static long s_hair_last_day = -1;
static bool s_hair_fired    = false;
static int  s_hair_interval = STUDY_HAIR_INTERVAL_DEFAULT;

void study_sched_hair_set_last_day(long last_day) {
    s_hair_last_day = last_day;
    s_hair_fired    = false;
}

int study_sched_hair_interval_days(void) { return s_hair_interval; }

void study_sched_hair_set_interval(int days) {
    if (days < STUDY_HAIR_INTERVAL_MIN) days = STUDY_HAIR_INTERVAL_MIN;
    if (days > STUDY_HAIR_INTERVAL_MAX) days = STUDY_HAIR_INTERVAL_MAX;
    s_hair_interval = days;
}

/* 下一次洗头发应在哪天（epoch 日序号）；从未洗过返回 -1 */
long study_sched_hair_next_epoch_day(void) {
    if (s_hair_last_day < 0) return -1L;
    return s_hair_last_day + s_hair_interval;
}

bool study_sched_hair_should_remind(long today_day, int now_h, int now_m) {
    if (s_hair_last_day < 0) return false;        /* 从未洗过头，无从提醒 */
    if (s_hair_fired) return false;               /* 本周期已提醒过 */
    if (today_day < s_hair_last_day + s_hair_interval) return false;  /* 未到洗头日 */

    /* 只在早晨固定窗口内触发一次 */
    int now  = now_h * 60 + now_m;
    int dueL = HAIR_HOUR * 60 + HAIR_MINUTE;
    int dueR = dueL + HAIR_WINDOW_MIN;
    if (now < dueL || now > dueR) return false;

    s_hair_fired = true;
    return true;
}

/* ---------- 工具函数 ---------- */
int study_sched_minutes_until(int from_h, int from_m, int to_h, int to_m) {
    int from = from_h * 60 + from_m;
    int to   = to_h   * 60 + to_m;
    int diff = to - from;
    if (diff < 0) diff += 24 * 60;     /* 跨日处理 */
    return diff;
}

/* ---------- 调度核心 ---------- */
bool study_sched_find_next(int now_h, int now_m, study_sched_ev_t *ev) {
    if (!ev) return false;

    /* TASK_MAX_COUNT=256 → 1KB 栈上数组。在 study_tick(原 3KB 栈) 上会把栈用爆
     * （实测 Guru Meditation stack protection fault，仅越界 ~20B）。改堆分配。 */
    int *ids = (int *)malloc(TASK_MAX_COUNT * sizeof(int));
    if (!ids) return false;
    int n = study_task_list_today(ids, TASK_MAX_COUNT);

    int best_id = -1;
    int best_due_h = -1, best_due_m = -1;
    int best_ordiff = 0x7fffffff;   /* 当前选中的 overdue 分钟差（小=优）*/

    int now = now_h * 60 + now_m;

    for (int i = 0; i < n; i++) {
        study_task_t t;
        if (study_task_get(ids[i], &t) != 0) continue;

        /* 没有时间点 → 不触发响铃提醒 */
        if (t.hour < 0 || t.minute < 0) continue;

        /* STUDY_REPEAT_ONCE 且 done → 已经做完，也不响铃 */
        if (t.repeat == STUDY_REPEAT_ONCE && t.done) continue;

        /* 今天已经响过 → 跳过 */
        if (fired_get(t.id)) continue;

        int due = t.hour * 60 + t.minute;
        int odiff = now - due;    /* 正值=过期 负值=未到 */

        /* 选择标准：优先选 overdue 最小的；
         * 如果都未到（odiff<0），选未到时间最近的（odiff 最大/最接近 0）。
         * 用"离 due 的绝对距离"做次级比较，最直观。 */
        int absdiff = odiff < 0 ? -odiff : odiff;

        bool better = false;
        if (best_id < 0) {
            better = true;
        } else {
            /* 先比「是否已经到点」：过期/当刻的比未到点的优先级高 */
            bool cur_is_due  = (odiff >= 0);
            bool best_is_due = (best_ordiff >= 0);
            if (cur_is_due != best_is_due) {
                better = cur_is_due;   /* 已到点的先处理 */
            } else if (cur_is_due) {
                /* 都已过期/当刻：选过期时间较短的 */
                better = (odiff < best_ordiff);
            } else {
                /* 都未到：选距离最近的 */
                better = (absdiff < (best_ordiff < 0 ? -best_ordiff : best_ordiff));
            }
        }

        if (better) {
            best_id = t.id;
            best_due_h = t.hour;
            best_due_m = t.minute;
            best_ordiff = odiff;
        }
    }

    if (best_id < 0) { free(ids); return false; }

    ev->kind = STUDY_SCHED_TASK_DUE;
    ev->task_id = best_id;
    ev->due_hour = (int8_t)best_due_h;
    ev->due_minute = (int8_t)best_due_m;
    ev->overdue_minutes = best_ordiff;
    free(ids);
    return true;
}

void study_sched_ack_fired(int task_id) {
    if (task_id > 0) fired_set(task_id, true);
}

/* ---------- 场景钩子：基于 title 关键词匹配 ---------- */
struct scene_kw {
    const char        *kw;
    study_sched_scene_t scene;
};
static const struct scene_kw s_scene_keywords[] = {
    /* 特化项在前，避免"睡前洗漱"被泛关键词"洗漱"误判成"早上洗漱"(§3.2) */
    { "早上洗漱",      STUDY_SCENE_MORNING_WASH },
    { "睡前洗漱",      STUDY_SCENE_NIGHT_WASH   },
    { "洗头发",        STUDY_SCENE_HAIR_WASH    },
    { "开始学习",      STUDY_SCENE_START_STUDY  },
    { "早饭",          STUDY_SCENE_START_STUDY  },
    { "午饭",          STUDY_SCENE_LUNCH        },
    { "午休",          STUDY_SCENE_LUNCH        },
    { "晚饭",          STUDY_SCENE_DINNER       },
    { "散步",          STUDY_SCENE_DINNER       },
    { "睡觉",          STUDY_SCENE_SLEEP        },
    { "回顾今日",      STUDY_SCENE_SLEEP        },
    { "洗漱",          STUDY_SCENE_MORNING_WASH },  /* 通用兜底放最后 */
};
#define N_KW (int)(sizeof(s_scene_keywords) / sizeof(s_scene_keywords[0]))

bool study_sched_scene_after_done(int task_id, study_sched_scene_t *scene) {
    if (!scene) return false;
    study_task_t t;
    if (study_task_get(task_id, &t) != 0) return false;

    /* 只看日常秩序类（CAT_DAILY） */
    if (t.category != CAT_DAILY) return false;

    for (int i = 0; i < N_KW; i++) {
        if (strstr(t.title, s_scene_keywords[i].kw) != NULL) {
            *scene = s_scene_keywords[i].scene;
            return true;
        }
    }
    return false;
}
