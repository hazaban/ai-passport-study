/*
 * study_group.c — 分组视图的筛选逻辑
 */
#include "study_group.h"
#include "study_category.h"
#include <stdlib.h>

static bool task_in_group(const study_task_t *t, study_group_t group) {
    bool is_daily = (t->category == CAT_DAILY);
    switch (group) {
        case STUDY_GROUP_DAILY_ORDER: return is_daily;
        case STUDY_GROUP_SUBJECTS:    return !is_daily;
        case STUDY_GROUP_ALL:         return true;
    }
    return false;
}

static bool task_done_match(const study_task_t *t, study_done_filter_t f) {
    switch (f) {
        case STUDY_DONE_PENDING: return !t->done;
        case STUDY_DONE_DONE:    return  t->done;
    }
    return false;
}

static int cmp_by_time_then_id(const study_task_t *a, const study_task_t *b) {
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

int study_group_list_today(study_group_t group,
                           study_done_filter_t done_filter,
                           int *out_ids, int max) {
    if (!out_ids || max <= 0) return 0;

    int all_ids[TASK_MAX_COUNT];
    int n = study_task_list_today(all_ids, TASK_MAX_COUNT);

    /* 1) 先过滤 group + done，把 task 对象暂存起来便于排序。
     * keep 用堆分配，避免在小栈任务（esp_timer）里栈溢出。 */
    study_task_t *keep = (study_task_t *)calloc(TASK_MAX_COUNT, sizeof(study_task_t));
    if (!keep) return 0;
    int kc = 0;
    for (int i = 0; i < n; i++) {
        study_task_t t;
        if (study_task_get(all_ids[i], &t) != 0) continue;
        if (!task_in_group(&t, group)) continue;
        if (!task_done_match(&t, done_filter)) continue;
        keep[kc++] = t;
    }

    /* 2) 插入排序 */
    for (int i = 1; i < kc; i++) {
        study_task_t key = keep[i];
        int j = i - 1;
        while (j >= 0 && cmp_by_time_then_id(&keep[j], &key) > 0) {
            keep[j + 1] = keep[j];
            j--;
        }
        keep[j + 1] = key;
    }

    /* 3) 截断输出 */
    int out_n = kc < max ? kc : max;
    for (int i = 0; i < out_n; i++) out_ids[i] = keep[i].id;
    free(keep);
    return out_n;
}

void study_group_count_today(study_group_t group, study_group_count_t *out) {
    if (!out) return;
    out->total = out->pending = out->done = 0;

    int all_ids[TASK_MAX_COUNT];
    int n = study_task_list_today(all_ids, TASK_MAX_COUNT);
    for (int i = 0; i < n; i++) {
        study_task_t t;
        if (study_task_get(all_ids[i], &t) != 0) continue;
        if (!task_in_group(&t, group)) continue;
        out->total++;
        if (t.done) out->done++;
        else out->pending++;
    }
}
