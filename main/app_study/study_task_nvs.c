/*
 * study_task_nvs.c — NVS 后端（条件编译：仅 ESP_PLATFORM 下有效；
 * 宿主编译时退化成"未实现但能通过链接"的存根，方便把 app_study.c
 * 的代码也一起做静态编译检查）。
 */
#include "study_task_nvs.h"
#include "study_category.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM

#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "esp_log.h"
static const char *TAG = "study_nvs";

static nvs_handle_t s_h_tasks;
static nvs_handle_t s_h_config;
static bool s_ready = false;

/* ---------- JSON 序列化 ---------- */
static int task_to_json(const study_task_t *t, char *out, int out_sz) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", t->id);
    cJSON_AddStringToObject(root, "title", t->title);
    cJSON_AddNumberToObject(root, "category", t->category);
    cJSON_AddNumberToObject(root, "subtype", t->subtype);
    cJSON_AddNumberToObject(root, "h", t->hour);
    cJSON_AddNumberToObject(root, "m", t->minute);
    cJSON_AddNumberToObject(root, "r", t->repeat);
    cJSON_AddBoolToObject(root,   "done", t->done);
    cJSON_AddNumberToObject(root, "done_at", t->done_at);
    cJSON_AddNumberToObject(root, "created_at", t->created_at);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return -1;
    size_t sl = strlen(s);
    if ((int)sl + 1 > out_sz) { free(s); return -1; }
    memcpy(out, s, sl + 1);
    free(s);
    return (int)sl;
}

static int json_to_task(const char *s, study_task_t *out) {
    cJSON *root = cJSON_Parse(s);
    if (!root) return -1;
    memset(out, 0, sizeof(*out));
    cJSON *j;
    j = cJSON_GetObjectItemCaseSensitive(root, "id"); out->id = j ? j->valueint : 0;
    j = cJSON_GetObjectItemCaseSensitive(root, "title");
    if (j && j->valuestring) {
        strncpy(out->title, j->valuestring, TASK_TITLE_LEN - 1);
    }
    j = cJSON_GetObjectItemCaseSensitive(root, "category"); out->category = j ? j->valueint : 0;
    j = cJSON_GetObjectItemCaseSensitive(root, "subtype");  out->subtype  = (uint8_t)(j ? j->valueint : 0);
    j = cJSON_GetObjectItemCaseSensitive(root, "h");        out->hour     = (int8_t)(j ? j->valueint : -1);
    j = cJSON_GetObjectItemCaseSensitive(root, "m");        out->minute   = (int8_t)(j ? j->valueint : -1);
    j = cJSON_GetObjectItemCaseSensitive(root, "r");        out->repeat   = (uint8_t)(j ? j->valueint : 0);
    j = cJSON_GetObjectItemCaseSensitive(root, "done");     out->done     = cJSON_IsTrue(j);
    j = cJSON_GetObjectItemCaseSensitive(root, "done_at");  out->done_at  = j ? (uint32_t)j->valuedouble : 0;
    j = cJSON_GetObjectItemCaseSensitive(root, "created_at"); out->created_at = j ? (uint32_t)j->valuedouble : 0;
    cJSON_Delete(root);
    return 0;
}

/* ---------- VTable 实现 ---------- */
static int nvs_save_task(const study_task_t *t) {
    char key[16];
    snprintf(key, sizeof(key), "task_%d", t->id);
    char buf[512];
    int len = task_to_json(t, buf, sizeof(buf));
    if (len <= 0) return ESP_FAIL;
    esp_err_t e = nvs_set_blob(s_h_tasks, key, buf, (size_t)len + 1);
    if (e != ESP_OK) return ESP_FAIL;
    return nvs_commit(s_h_tasks) == ESP_OK ? 0 : -1;
}

static int nvs_load_task(int id, study_task_t *out) {
    char key[16];
    snprintf(key, sizeof(key), "task_%d", id);
    char buf[512]; size_t sz = sizeof(buf);
    esp_err_t e = nvs_get_blob(s_h_tasks, key, buf, &sz);
    if (e != ESP_OK || sz < 2) return -1;
    return json_to_task(buf, out);
}

static int nvs_delete_task(int id) {
    char key[16];
    snprintf(key, sizeof(key), "task_%d", id);
    esp_err_t e = nvs_erase_key(s_h_tasks, key);
    if (e != ESP_OK) return -1;
    nvs_commit(s_h_tasks);
    return 0;
}

static int nvs_all_ids(int *out_ids, int max, int *out_count) {
    /* 遍历：从 next_id-1 开始往回扫，每个 task_<i> 尝试 load；
     * 效率足够（<256 任务），且不需要迭代 NVS 键 */
    uint32_t next = 0;
    nvs_get_u32(s_h_tasks, "next_id", &next);     /* 不存在也 OK */
    int got = 0;
    for (uint32_t i = 1; i <= next && got < max; i++) {
        study_task_t tmp;
        if (nvs_load_task((int)i, &tmp) == 0) {
            out_ids[got++] = (int)i;
        }
    }
    *out_count = got;
    return 0;
}

static int nvs_next_id(void) {
    uint32_t v = 1;
    nvs_get_u32(s_h_tasks, "next_id", &v);
    v++;
    nvs_set_u32(s_h_tasks, "next_id", v);
    nvs_commit(s_h_tasks);
    return (int)v;
}

static int nvs_load_meta_int(const char *k, int def) {
    if (!k) return def;
    int32_t v = (int32_t)def;
    if (nvs_get_i32(s_h_config, k, (int32_t *)&v) != ESP_OK) return def;
    return (int)v;
}
static int nvs_save_meta_int(const char *k, int v) {
    if (!k) return -1;
    esp_err_t e = nvs_set_i32(s_h_config, k, (int32_t)v);
    if (e != ESP_OK) return -1;
    return nvs_commit(s_h_config) == ESP_OK ? 0 : -1;
}

static study_task_store_t s_store = {
    .save_task     = nvs_save_task,
    .load_task     = nvs_load_task,
    .delete_task   = nvs_delete_task,
    .all_task_ids  = nvs_all_ids,
    .next_id       = nvs_next_id,
    .load_meta_int = nvs_load_meta_int,
    .save_meta_int = nvs_save_meta_int,
};

int study_task_nvs_init(void) {
    esp_err_t e;
    if ((e = nvs_open("tasks",  NVS_READWRITE, &s_h_tasks))  != ESP_OK) {
        ESP_LOGE(TAG, "nvs open tasks failed: %s", esp_err_to_name(e)); return -1;
    }
    if ((e = nvs_open("config", NVS_READWRITE, &s_h_config)) != ESP_OK) {
        ESP_LOGE(TAG, "nvs open config failed: %s", esp_err_to_name(e)); return -1;
    }
    s_ready = true;
    return 0;
}

const study_task_store_t *study_task_nvs_store(void) {
    return s_ready ? &s_store : NULL;
}

#else  /* !ESP_PLATFORM — 宿主编译存根 */

int study_task_nvs_init(void) { return 0; }
const study_task_store_t *study_task_nvs_store(void) { return NULL; }

#endif
