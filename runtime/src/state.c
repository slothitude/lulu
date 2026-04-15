#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "state.h"
#include "cJSON.h"

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
                    cJSON *args, cJSON *result, int success) {
    FILE *f = fopen(log_path, "a");
    if (!f) return;

    cJSON *entry = cJSON_CreateObject();
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
