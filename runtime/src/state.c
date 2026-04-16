#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "state.h"
#include "agent_db.h"
#include "cJSON.h"

extern AgentDB g_adb;

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
    if (!buf) { fclose(f); return NULL; }

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
        fclose(f); remove(tmp); return 0;
    }
    fclose(f);

    remove(path);
    if (rename(tmp, path) != 0) { remove(tmp); return 0; }
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

    if (cfg->max_iterations <= 0) cfg->max_iterations = 10;
    if (cfg->max_retries <= 0) cfg->max_retries = 3;
    if (cfg->http_timeout_ms <= 0) cfg->http_timeout_ms = 30000;

    cJSON_Delete(root);
    return 1;
}

/* ========================= Memory (graph-backed) ========================= */

void memory_init(WorkingMemory *mem) {
    memset(mem, 0, sizeof(WorkingMemory));
}

int memory_load(WorkingMemory *mem, const char *path) {
    (void)path;
    /* In v4, memory lives in the graph. Load scalar values into the
       WorkingMemory struct for compatibility with v3 code paths. */
    char *val;

    val = agent_db_memory_get_scalar(&g_adb, "step_count");
    if (val && strcmp(val, "NULL") != 0) mem->step_count = atoi(val);
    free(val);

    val = agent_db_memory_get_scalar(&g_adb, "total_messages");
    if (val && strcmp(val, "NULL") != 0) mem->total_messages = strtoll(val, NULL, 10);
    free(val);

    val = agent_db_memory_get_scalar(&g_adb, "summary");
    if (val && strcmp(val, "NULL") != 0) {
        strncpy(mem->summary, val, sizeof(mem->summary) - 1);
    }
    free(val);

    val = agent_db_memory_get_scalar(&g_adb, "last_tool");
    if (val && strcmp(val, "NULL") != 0) {
        strncpy(mem->last_tool, val, sizeof(mem->last_tool) - 1);
    }
    free(val);

    val = agent_db_memory_get_scalar(&g_adb, "last_result");
    if (val && strcmp(val, "NULL") != 0) {
        strncpy(mem->last_result, val, sizeof(mem->last_result) - 1);
    }
    free(val);

    return 1;
}

int memory_save(WorkingMemory *mem, const char *path) {
    (void)path;
    /* In v4, memory writes go to the graph immediately via set_scalar. */
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", mem->step_count);
    agent_db_memory_set_scalar(&g_adb, "step_count", buf);

    snprintf(buf, sizeof(buf), "%lld", mem->total_messages);
    agent_db_memory_set_scalar(&g_adb, "total_messages", buf);

    if (mem->summary[0])
        agent_db_memory_set_scalar(&g_adb, "summary", mem->summary);
    if (mem->last_tool[0])
        agent_db_memory_set_scalar(&g_adb, "last_tool", mem->last_tool);
    if (mem->last_result[0])
        agent_db_memory_set_scalar(&g_adb, "last_result", mem->last_result);

    return 1;
}

void memory_update(WorkingMemory *mem, const char *tool, const char *result, int success) {
    mem->step_count++;
    agent_db_memory_increment(&g_adb, "step_count");

    strncpy(mem->last_tool, tool ? tool : "", sizeof(mem->last_tool) - 1);
    if (result) strncpy(mem->last_result, result, sizeof(mem->last_result) - 1);

    agent_db_memory_set_scalar(&g_adb, "last_tool", mem->last_tool);
    agent_db_memory_set_scalar(&g_adb, "last_result", mem->last_result);

    /* Track errors in graph */
    if (!success && result) {
        agent_db_memory_add(&g_adb, "error", "", result);
    }
}

void memory_track_file(WorkingMemory *mem, const char *path) {
    if (mem->files_count < MAX_MEMORY_FILES) {
        strncpy(mem->files_created[mem->files_count], path, 255);
        mem->files_count++;
    }
    agent_db_memory_add(&g_adb, "file", path, path);
}

/* ========================= JSONL Logging (→ graph LOG_EVENT) ========================= */

void state_log_step(const char *log_path, int step_id, const char *tool,
                    cJSON *args, cJSON *result, int success,
                    int iteration, const char *stage) {
    /* Log to graph */
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "tool", tool ? tool : "none");
    cJSON_AddNumberToObject(entry, "step", step_id);
    if (args) cJSON_AddItemToObject(entry, "args", cJSON_Duplicate(args, 1));
    if (result) cJSON_AddItemToObject(entry, "result", cJSON_Duplicate(result, 1));
    char *json = cJSON_PrintUnformatted(entry);
    agent_db_log_event(&g_adb, "tool_step", iteration, stage ? stage : "actor",
                       json, success);
    free(json);
    cJSON_Delete(entry);

    /* Also append to JSONL file for backward compat */
    FILE *f = fopen(log_path, "a");
    if (!f) return;

    char ts[32]; get_iso8601(ts, sizeof(ts));
    cJSON *fentry = cJSON_CreateObject();
    cJSON_AddStringToObject(fentry, "ts", ts);
    cJSON_AddNumberToObject(fentry, "iter", iteration);
    cJSON_AddStringToObject(fentry, "stage", stage ? stage : "actor");
    cJSON_AddNumberToObject(fentry, "step", step_id);
    cJSON_AddStringToObject(fentry, "tool", tool ? tool : "none");
    cJSON_AddBoolToObject(fentry, "success", success);
    if (args) cJSON_AddItemToObject(fentry, "args", cJSON_Duplicate(args, 1));
    if (result) cJSON_AddItemToObject(fentry, "result", cJSON_Duplicate(result, 1));

    char *fjson = cJSON_PrintUnformatted(fentry);
    fprintf(f, "%s\n", fjson);
    free(fjson); cJSON_Delete(fentry); fclose(f);
}

void state_log_stage(const char *log_path, int iteration, const char *stage,
                     const char *summary, int success) {
    agent_db_log_event(&g_adb, "stage", iteration, stage ? stage : "unknown",
                       summary, success);

    FILE *f = fopen(log_path, "a");
    if (!f) return;
    char ts[32]; get_iso8601(ts, sizeof(ts));
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "ts", ts);
    cJSON_AddNumberToObject(entry, "iter", iteration);
    cJSON_AddStringToObject(entry, "stage", stage ? stage : "unknown");
    cJSON_AddStringToObject(entry, "summary", summary ? summary : "");
    cJSON_AddBoolToObject(entry, "success", success);
    char *json = cJSON_PrintUnformatted(entry);
    fprintf(f, "%s\n", json);
    free(json); cJSON_Delete(entry); fclose(f);
}

void state_log_llm(const char *log_path, int iteration, const char *stage,
                   const char *prompt_summary, const char *raw_response,
                   const char *parsed_json, int success,
                   const char *prompt_hash, int cache_hit) {
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "prompt_summary", prompt_summary ? prompt_summary : "");
    if (prompt_hash) cJSON_AddStringToObject(entry, "prompt_hash", prompt_hash);
    cJSON_AddBoolToObject(entry, "cache_hit", cache_hit ? 1 : 0);
    char *json = cJSON_PrintUnformatted(entry);
    agent_db_log_event(&g_adb, "llm", iteration, stage ? stage : "unknown",
                       json, success);
    free(json); cJSON_Delete(entry);

    /* JSONL file */
    FILE *f = fopen(log_path, "a");
    if (!f) return;
    char ts[32]; get_iso8601(ts, sizeof(ts));
    cJSON *fentry = cJSON_CreateObject();
    cJSON_AddStringToObject(fentry, "ts", ts);
    cJSON_AddNumberToObject(fentry, "iter", iteration);
    cJSON_AddStringToObject(fentry, "stage", stage ? stage : "unknown");
    cJSON_AddStringToObject(fentry, "type", "llm");
    cJSON_AddBoolToObject(fentry, "success", success);
    if (prompt_hash && prompt_hash[0])
        cJSON_AddStringToObject(fentry, "prompt_hash", prompt_hash);
    cJSON_AddBoolToObject(fentry, "cache_hit", cache_hit ? 1 : 0);
    if (prompt_summary) {
        char buf[201]; size_t len = strlen(prompt_summary);
        if (len > 200) len = 200;
        memcpy(buf, prompt_summary, len); buf[len] = 0;
        cJSON_AddStringToObject(fentry, "prompt_summary", buf);
    }
    if (raw_response) {
        size_t rlen = strlen(raw_response);
        if (rlen > LLM_LOG_MAX) rlen = LLM_LOG_MAX;
        char *trunc = (char *)malloc(rlen + 1);
        memcpy(trunc, raw_response, rlen); trunc[rlen] = 0;
        cJSON_AddStringToObject(fentry, "llm_raw_trunc", trunc);
        free(trunc);
    }
    if (parsed_json) cJSON_AddStringToObject(fentry, "parsed_json", parsed_json);
    char *fjson = cJSON_PrintUnformatted(fentry);
    fprintf(f, "%s\n", fjson);
    free(fjson); cJSON_Delete(fentry); fclose(f);
}
