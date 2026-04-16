#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "state.h"
#include "cJSON.h"

/* ========================= ISO 8601 Timestamp ========================= */

static void get_iso8601(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *t = gmtime(&now);
    snprintf(buf, size, "%04d-%02d-%02dT%02d:%02d:%02dZ",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
}

/* ========================= File I/O ========================= */

char *read_file_contents(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc(size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    fread(buf, 1, size, f);
    buf[size] = 0;
    fclose(f);
    return buf;
}

int write_file_atomic(const char *path, const char *content) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;

    size_t len = strlen(content);
    if (fwrite(content, 1, len, f) != len) {
        fclose(f);
        remove(tmp);
        return 0;
    }
    fclose(f);

    if (remove(path) != 0 && errno != ENOENT) {
        /* File exists and can't be removed */
    }

    if (rename(tmp, path) != 0) {
        remove(tmp);
        return 0;
    }

    return 1;
}

/* ========================= Config ========================= */

int config_load(Config *cfg, const char *path) {
    memset(cfg, 0, sizeof(Config));

    char *data = read_file_contents(path);
    if (!data) return 0;

    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!root) return 0;

    cJSON *item;

    item = cJSON_GetObjectItem(root, "provider");
    if (cJSON_IsString(item)) strncpy(cfg->provider, item->valuestring, sizeof(cfg->provider) - 1);

    item = cJSON_GetObjectItem(root, "model");
    if (cJSON_IsString(item)) strncpy(cfg->model, item->valuestring, sizeof(cfg->model) - 1);

    item = cJSON_GetObjectItem(root, "endpoint");
    if (cJSON_IsString(item)) strncpy(cfg->endpoint, item->valuestring, sizeof(cfg->endpoint) - 1);

    item = cJSON_GetObjectItem(root, "apikey");
    if (cJSON_IsString(item)) strncpy(cfg->apikey, item->valuestring, sizeof(cfg->apikey) - 1);

    item = cJSON_GetObjectItem(root, "max_iterations");
    if (cJSON_IsNumber(item)) cfg->max_iterations = item->valueint;

    item = cJSON_GetObjectItem(root, "max_retries");
    if (cJSON_IsNumber(item)) cfg->max_retries = item->valueint;

    item = cJSON_GetObjectItem(root, "temperature");
    if (cJSON_IsNumber(item)) cfg->temperature = item->valuedouble;

    item = cJSON_GetObjectItem(root, "http_timeout_ms");
    if (cJSON_IsNumber(item)) cfg->http_timeout_ms = item->valueint;

    /* Defaults */
    if (cfg->max_iterations <= 0) cfg->max_iterations = 10;
    if (cfg->max_retries <= 0) cfg->max_retries = 3;
    if (cfg->http_timeout_ms <= 0) cfg->http_timeout_ms = 30000;

    cJSON_Delete(root);
    return 1;
}

/* ========================= Memory ========================= */

void memory_init(WorkingMemory *mem) {
    memset(mem, 0, sizeof(WorkingMemory));
}

int memory_load(WorkingMemory *mem, const char *path) {
    char *data = read_file_contents(path);
    if (!data) return 0;

    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!root) return 0;

    cJSON *files = cJSON_GetObjectItem(root, "files_created");
    if (cJSON_IsArray(files)) {
        int count = cJSON_GetArraySize(files);
        if (count > MAX_MEMORY_FILES) count = MAX_MEMORY_FILES;
        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(files, i);
            if (cJSON_IsString(item)) {
                strncpy(mem->files_created[i], item->valuestring, 255);
            }
        }
        mem->files_count = count;
    }

    cJSON *errors = cJSON_GetObjectItem(root, "known_errors");
    if (cJSON_IsArray(errors)) {
        int count = cJSON_GetArraySize(errors);
        if (count > MAX_MEMORY_ERRORS) count = MAX_MEMORY_ERRORS;
        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(errors, i);
            if (cJSON_IsString(item)) {
                strncpy(mem->known_errors[i], item->valuestring, 255);
            }
        }
        mem->errors_count = count;
    }

    cJSON *summary = cJSON_GetObjectItem(root, "summary");
    if (cJSON_IsString(summary)) {
        strncpy(mem->summary, summary->valuestring, sizeof(mem->summary) - 1);
    }

    cJSON *last_tool = cJSON_GetObjectItem(root, "last_tool");
    if (cJSON_IsString(last_tool)) {
        strncpy(mem->last_tool, last_tool->valuestring, sizeof(mem->last_tool) - 1);
    }

    cJSON *last_result = cJSON_GetObjectItem(root, "last_result");
    if (cJSON_IsString(last_result)) {
        strncpy(mem->last_result, last_result->valuestring, sizeof(mem->last_result) - 1);
    }

    cJSON *step_count = cJSON_GetObjectItem(root, "step_count");
    if (cJSON_IsNumber(step_count)) mem->step_count = step_count->valueint;

    /* Goals */
    cJSON *goals = cJSON_GetObjectItem(root, "goals");
    if (cJSON_IsArray(goals)) {
        int count = cJSON_GetArraySize(goals);
        if (count > MAX_GOALS) count = MAX_GOALS;
        for (int i = 0; i < count; i++) {
            cJSON *g = cJSON_GetArrayItem(goals, i);
            Goal *gl = &mem->goals[i];
            cJSON *f;
            f = cJSON_GetObjectItem(g, "id");
            if (cJSON_IsString(f)) strncpy(gl->id, f->valuestring, sizeof(gl->id) - 1);
            f = cJSON_GetObjectItem(g, "text");
            if (cJSON_IsString(f)) strncpy(gl->text, f->valuestring, sizeof(gl->text) - 1);
            f = cJSON_GetObjectItem(g, "status");
            if (cJSON_IsString(f)) strncpy(gl->status, f->valuestring, sizeof(gl->status) - 1);
            f = cJSON_GetObjectItem(g, "created_at");
            if (cJSON_IsNumber(f)) gl->created_at = (time_t)f->valuedouble;
        }
        mem->goals_count = count;
    }

    cJSON *total_msgs = cJSON_GetObjectItem(root, "total_messages");
    if (cJSON_IsNumber(total_msgs)) mem->total_messages = (long long)total_msgs->valuedouble;

    cJSON_Delete(root);
    return 1;
}

int memory_save(WorkingMemory *mem, const char *path) {
    cJSON *root = cJSON_CreateObject();

    cJSON *files = cJSON_CreateArray();
    for (int i = 0; i < mem->files_count && i < MAX_MEMORY_FILES; i++) {
        cJSON_AddItemToArray(files, cJSON_CreateString(mem->files_created[i]));
    }
    cJSON_AddItemToObject(root, "files_created", files);

    cJSON *errors = cJSON_CreateArray();
    for (int i = 0; i < mem->errors_count && i < MAX_MEMORY_ERRORS; i++) {
        cJSON_AddItemToArray(errors, cJSON_CreateString(mem->known_errors[i]));
    }
    cJSON_AddItemToObject(root, "known_errors", errors);

    cJSON_AddStringToObject(root, "summary", mem->summary);
    cJSON_AddStringToObject(root, "last_tool", mem->last_tool);
    cJSON_AddStringToObject(root, "last_result", mem->last_result);
    cJSON_AddNumberToObject(root, "step_count", mem->step_count);

    /* Goals */
    cJSON *goals_arr = cJSON_CreateArray();
    for (int i = 0; i < mem->goals_count && i < MAX_GOALS; i++) {
        cJSON *g = cJSON_CreateObject();
        cJSON_AddStringToObject(g, "id", mem->goals[i].id);
        cJSON_AddStringToObject(g, "text", mem->goals[i].text);
        cJSON_AddStringToObject(g, "status", mem->goals[i].status);
        cJSON_AddNumberToObject(g, "created_at", (double)mem->goals[i].created_at);
        cJSON_AddItemToArray(goals_arr, g);
    }
    cJSON_AddItemToObject(root, "goals", goals_arr);
    cJSON_AddNumberToObject(root, "total_messages", (double)mem->total_messages);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    int ok = write_file_atomic(path, json);
    free(json);
    return ok;
}

void memory_update(WorkingMemory *mem, const char *tool, const char *result, int success) {
    mem->step_count++;

    strncpy(mem->last_tool, tool ? tool : "", sizeof(mem->last_tool) - 1);
    if (result) {
        strncpy(mem->last_result, result, sizeof(mem->last_result) - 1);
    }

    /* Track created files */
    if (strcmp(tool, "create_file") == 0 && success) {
        if (mem->files_count < MAX_MEMORY_FILES) {
            /* We'd need the path from args, but we'll track from the result */
        }
    }

    /* Track errors */
    if (!success && mem->errors_count < MAX_MEMORY_ERRORS) {
        strncpy(mem->known_errors[mem->errors_count], result ? result : "unknown error", 255);
        mem->errors_count++;
    }
}

void memory_track_file(WorkingMemory *mem, const char *path) {
    if (mem->files_count < MAX_MEMORY_FILES) {
        strncpy(mem->files_created[mem->files_count], path, 255);
        mem->files_count++;
    } else {
        /* Shift and add */
        for (int i = 1; i < MAX_MEMORY_FILES; i++) {
            strncpy(mem->files_created[i - 1], mem->files_created[i], 255);
        }
        strncpy(mem->files_created[MAX_MEMORY_FILES - 1], path, 255);
    }
}

/* ========================= JSONL Logging ========================= */

void state_log_step(const char *log_path, int step_id, const char *tool,
                    cJSON *args, cJSON *result, int success,
                    int iteration, const char *stage) {
    FILE *f = fopen(log_path, "a");
    if (!f) return;

    char ts[32];
    get_iso8601(ts, sizeof(ts));

    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "ts", ts);
    cJSON_AddNumberToObject(entry, "iter", iteration);
    cJSON_AddStringToObject(entry, "stage", stage ? stage : "actor");
    cJSON_AddNumberToObject(entry, "step", step_id);
    cJSON_AddStringToObject(entry, "tool", tool ? tool : "none");
    cJSON_AddBoolToObject(entry, "success", success);

    if (args) {
        cJSON *args_copy = cJSON_Duplicate(args, 1);
        cJSON_AddItemToObject(entry, "args", args_copy);
    }
    if (result) {
        cJSON *res_copy = cJSON_Duplicate(result, 1);
        cJSON_AddItemToObject(entry, "result", res_copy);
    }

    char *json = cJSON_PrintUnformatted(entry);
    fprintf(f, "%s\n", json);

    free(json);
    cJSON_Delete(entry);
    fclose(f);
}

void state_log_stage(const char *log_path, int iteration, const char *stage,
                     const char *summary, int success) {
    FILE *f = fopen(log_path, "a");
    if (!f) return;

    char ts[32];
    get_iso8601(ts, sizeof(ts));

    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "ts", ts);
    cJSON_AddNumberToObject(entry, "iter", iteration);
    cJSON_AddStringToObject(entry, "stage", stage ? stage : "unknown");
    cJSON_AddStringToObject(entry, "summary", summary ? summary : "");
    cJSON_AddBoolToObject(entry, "success", success);

    char *json = cJSON_PrintUnformatted(entry);
    fprintf(f, "%s\n", json);

    free(json);
    cJSON_Delete(entry);
    fclose(f);
}

void state_log_llm(const char *log_path, int iteration, const char *stage,
                   const char *prompt_summary, const char *raw_response,
                   const char *parsed_json, int success,
                   const char *prompt_hash, int cache_hit) {
    FILE *f = fopen(log_path, "a");
    if (!f) return;

    char ts[32];
    get_iso8601(ts, sizeof(ts));

    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "ts", ts);
    cJSON_AddNumberToObject(entry, "iter", iteration);
    cJSON_AddStringToObject(entry, "stage", stage ? stage : "unknown");
    cJSON_AddStringToObject(entry, "type", "llm");
    cJSON_AddBoolToObject(entry, "success", success);
    cJSON_AddBoolToObject(entry, "parse_success", parsed_json ? 1 : 0);

    if (prompt_hash && prompt_hash[0])
        cJSON_AddStringToObject(entry, "prompt_hash", prompt_hash);
    cJSON_AddBoolToObject(entry, "cache_hit", cache_hit ? 1 : 0);

    /* Truncate prompt summary to 200 chars */
    if (prompt_summary) {
        char buf[201];
        size_t len = strlen(prompt_summary);
        if (len > 200) len = 200;
        memcpy(buf, prompt_summary, len);
        buf[len] = 0;
        cJSON_AddStringToObject(entry, "prompt_summary", buf);
    }

    /* Truncate raw response to LLM_LOG_MAX */
    if (raw_response) {
        size_t raw_len = strlen(raw_response);
        if (raw_len > LLM_LOG_MAX) {
            char *trunc = (char *)malloc(LLM_LOG_MAX + 1);
            if (trunc) {
                memcpy(trunc, raw_response, LLM_LOG_MAX);
                trunc[LLM_LOG_MAX] = 0;
                cJSON_AddStringToObject(entry, "llm_raw_trunc", trunc);
                free(trunc);
            }
        } else {
            cJSON_AddStringToObject(entry, "llm_raw", raw_response);
        }
    }

    if (parsed_json) {
        cJSON_AddStringToObject(entry, "parsed_json", parsed_json);
    }

    char *json = cJSON_PrintUnformatted(entry);
    fprintf(f, "%s\n", json);

    free(json);
    cJSON_Delete(entry);
    fclose(f);
}
