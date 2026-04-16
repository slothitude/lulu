#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tasks.h"
#include "state.h"    /* for read_file_contents, write_file_atomic */
#include "cJSON.h"

/* ===== Internal storage ===== */

static Task g_tasks[MAX_TASKS];
static int  g_task_count = 0;
static char g_tasks_path[512] = {0};
static int  g_next_id = 1;

/* ===== Helpers ===== */

static void task_set_id(Task *t) {
    snprintf(t->id, TASK_ID_MAX, "task_%d", g_next_id++);
}

/* ===== Load / Save ===== */

void tasks_load(const char *path) {
    strncpy(g_tasks_path, path, sizeof(g_tasks_path) - 1);
    g_task_count = 0;
    g_next_id = 1;

    char *data = read_file_contents(path);
    if (!data) return;

    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!root) return;

    cJSON *next_id_item = cJSON_GetObjectItem(root, "next_id");
    if (cJSON_IsNumber(next_id_item)) g_next_id = next_id_item->valueint;

    cJSON *arr = cJSON_GetObjectItem(root, "tasks");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); return; }

    int count = cJSON_GetArraySize(arr);
    if (count > MAX_TASKS) count = MAX_TASKS;

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        Task *t = &g_tasks[g_task_count];
        memset(t, 0, sizeof(Task));

        cJSON *f;

        f = cJSON_GetObjectItem(item, "id");
        if (cJSON_IsString(f)) strncpy(t->id, f->valuestring, TASK_ID_MAX - 1);

        f = cJSON_GetObjectItem(item, "status");
        if (cJSON_IsString(f)) strncpy(t->status, f->valuestring, TASK_STATUS_MAX - 1);
        else strncpy(t->status, "pending", TASK_STATUS_MAX - 1);

        f = cJSON_GetObjectItem(item, "priority");
        if (cJSON_IsNumber(f)) t->priority = f->valueint;

        f = cJSON_GetObjectItem(item, "source");
        if (cJSON_IsString(f)) strncpy(t->source, f->valuestring, TASK_SOURCE_MAX - 1);

        f = cJSON_GetObjectItem(item, "chat_id");
        if (cJSON_IsNumber(f)) t->chat_id = (long long)f->valuedouble;

        f = cJSON_GetObjectItem(item, "prompt");
        if (cJSON_IsString(f)) strncpy(t->prompt, f->valuestring, TASK_PROMPT_MAX - 1);

        f = cJSON_GetObjectItem(item, "result");
        if (cJSON_IsString(f)) strncpy(t->result, f->valuestring, TASK_RESULT_MAX - 1);

        f = cJSON_GetObjectItem(item, "state");
        if (cJSON_IsString(f)) strncpy(t->state, f->valuestring, TASK_STATE_MAX - 1);

        f = cJSON_GetObjectItem(item, "last_error");
        if (cJSON_IsString(f)) strncpy(t->last_error, f->valuestring, TASK_ERROR_MAX - 1);

        f = cJSON_GetObjectItem(item, "plan");
        if (cJSON_IsString(f)) strncpy(t->plan, f->valuestring, TASK_PLAN_MAX - 1);

        f = cJSON_GetObjectItem(item, "attempts");
        if (cJSON_IsNumber(f)) t->attempts = f->valueint;

        f = cJSON_GetObjectItem(item, "max_attempts");
        if (cJSON_IsNumber(f)) t->max_attempts = f->valueint;
        else t->max_attempts = 3;

        f = cJSON_GetObjectItem(item, "created_at");
        if (cJSON_IsNumber(f)) t->created_at = (time_t)f->valuedouble;

        f = cJSON_GetObjectItem(item, "updated_at");
        if (cJSON_IsNumber(f)) t->updated_at = (time_t)f->valuedouble;

        g_task_count++;
    }

    cJSON_Delete(root);
}

void tasks_save(void) {
    if (!g_tasks_path[0]) return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "next_id", g_next_id);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < g_task_count; i++) {
        Task *t = &g_tasks[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", t->id);
        cJSON_AddStringToObject(item, "status", t->status);
        cJSON_AddNumberToObject(item, "priority", t->priority);
        cJSON_AddStringToObject(item, "source", t->source);
        cJSON_AddNumberToObject(item, "chat_id", (double)t->chat_id);
        cJSON_AddStringToObject(item, "prompt", t->prompt);
        cJSON_AddStringToObject(item, "result", t->result);
        cJSON_AddStringToObject(item, "state", t->state);
        cJSON_AddStringToObject(item, "last_error", t->last_error);
        cJSON_AddStringToObject(item, "plan", t->plan);
        cJSON_AddNumberToObject(item, "attempts", t->attempts);
        cJSON_AddNumberToObject(item, "max_attempts", t->max_attempts);
        cJSON_AddNumberToObject(item, "created_at", (double)t->created_at);
        cJSON_AddNumberToObject(item, "updated_at", (double)t->updated_at);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(root, "tasks", arr);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    write_file_atomic(g_tasks_path, json);
    free(json);
}

/* ===== CRUD ===== */

Task *tasks_create(const char *prompt, const char *source, long long chat_id, int priority) {
    if (g_task_count >= MAX_TASKS) return NULL;

    Task *t = &g_tasks[g_task_count++];
    memset(t, 0, sizeof(Task));

    task_set_id(t);
    strncpy(t->status, "pending", TASK_STATUS_MAX - 1);
    t->priority = priority;
    strncpy(t->source, source, TASK_SOURCE_MAX - 1);
    t->chat_id = chat_id;
    strncpy(t->prompt, prompt, TASK_PROMPT_MAX - 1);
    t->max_attempts = 3;
    t->created_at = time(NULL);
    t->updated_at = t->created_at;

    return t;
}

/* Priority + cooldown scheduler.
   - "pending" tasks first (highest priority)
   - "failed" tasks after cooldown (60s per attempt)
   - skip "running", "done" */
Task *tasks_next_runnable(void) {
    Task *best = NULL;
    time_t now = time(NULL);

    for (int i = 0; i < g_task_count; i++) {
        Task *t = &g_tasks[i];

        if (strcmp(t->status, "done") == 0) continue;
        if (strcmp(t->status, "running") == 0) continue;

        if (strcmp(t->status, "failed") == 0) {
            /* Cooldown: wait longer for each retry */
            int cooldown = 30 * t->attempts;
            if (t->max_attempts > 0 && t->attempts >= t->max_attempts) continue;
            if (difftime(now, t->updated_at) < cooldown) continue;
        }

        if (!best || t->priority > best->priority) {
            best = t;
        }
    }

    return best;
}

void tasks_update(Task *t, const char *status, const char *result) {
    if (!t) return;
    strncpy(t->status, status, TASK_STATUS_MAX - 1);
    if (result) strncpy(t->result, result, TASK_RESULT_MAX - 1);
    t->updated_at = time(NULL);
    tasks_save();
}

void tasks_append_state(Task *t, const char *text) {
    if (!t || !text) return;
    size_t cur = strlen(t->state);
    size_t add = strlen(text);
    /* Keep last TASK_STATE_MAX bytes */
    if (cur + add + 2 >= TASK_STATE_MAX) {
        size_t keep = TASK_STATE_MAX - add - 2;
        if (keep < cur) {
            memmove(t->state, t->state + (cur - keep), keep);
            t->state[keep] = 0;
        }
    }
    if (t->state[0]) strcat(t->state, "\n");
    strncat(t->state, text, TASK_STATE_MAX - strlen(t->state) - 1);
}

int tasks_count(const char *status) {
    int count = 0;
    for (int i = 0; i < g_task_count; i++) {
        if (!status || strcmp(g_tasks[i].status, status) == 0)
            count++;
    }
    return count;
}

Task *tasks_find(const char *id) {
    for (int i = 0; i < g_task_count; i++) {
        if (strcmp(g_tasks[i].id, id) == 0)
            return &g_tasks[i];
    }
    return NULL;
}

void tasks_list(char *buf, size_t buf_size) {
    buf[0] = 0;
    for (int i = 0; i < g_task_count; i++) {
        Task *t = &g_tasks[i];
        char line[256];
        snprintf(line, sizeof(line), "%s [%s] pri=%d att=%d/%d | %s\n",
                 t->id, t->status, t->priority, t->attempts, t->max_attempts,
                 t->prompt[0] ? t->prompt : "(no prompt)");
        if (strlen(buf) + strlen(line) < buf_size - 1) {
            strcat(buf, line);
        }
    }
    if (buf[0] == 0) strncpy(buf, "No tasks.\n", buf_size - 1);
}
