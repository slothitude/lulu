#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "critic.h"
#include "llm.h"
#include "state.h"
#include "agent_config.h"
#include "agent_core.h"

extern AgentConfig g_agent_cfg;

static CriticResult default_critic_result(void) {
    CriticResult r = {0};
    strncpy(r.status, "continue", sizeof(r.status) - 1);
    r.progress = 0.0;
    r.confidence = 0.0;
    return r;
}

static char *build_critic_context(const char *log_path, WorkingMemory *mem, const char *goal) {
    int tail_bytes = g_agent_cfg.behavior.critic_log_tail_bytes;
    if (tail_bytes <= 0) tail_bytes = 2048;

    /* Read the last portion of log */
    char *log_data = read_file_contents(log_path);
    char *escaped_log;
    if (log_data) {
        size_t len = strlen(log_data);
        if ((int)len > tail_bytes) {
            escaped_log = llm_escape_json_string(log_data + len - tail_bytes);
        } else {
            escaped_log = llm_escape_json_string(log_data);
        }
        free(log_data);
    } else {
        escaped_log = llm_escape_json_string("No log entries yet.");
    }

    char *escaped_goal = llm_escape_json_string(goal);
    char *escaped_summary = llm_escape_json_string(mem->summary[0] ? mem->summary : "None");

    /* Build file list */
    char files_str[2048] = "None";
    if (mem->files_count > 0) {
        files_str[0] = 0;
        for (int i = 0; i < mem->files_count; i++) {
            if (i > 0) strcat(files_str, ", ");
            strncat(files_str, mem->files_created[i], sizeof(files_str) - strlen(files_str) - 1);
        }
    }

    char *escaped_files = llm_escape_json_string(files_str);

    size_t size = 8192 + strlen(escaped_log) + strlen(escaped_goal) +
                  strlen(escaped_summary) + strlen(escaped_files);

    char *context = (char *)malloc(size);
    if (!context) {
        free(escaped_log); free(escaped_goal); free(escaped_summary); free(escaped_files);
        return NULL;
    }

    snprintf(context, size,
        "GOAL: %s\n\n"
        "FILES CREATED SO FAR: %s\n"
        "STEPS COMPLETED: %d\n"
        "WORK SUMMARY: %s\n\n"
        "RECENT LOG:\n%s",
        escaped_goal,
        escaped_files,
        mem->step_count,
        escaped_summary,
        escaped_log);

    free(escaped_log); free(escaped_goal); free(escaped_summary); free(escaped_files);
    return context;
}

CriticResult critic_run(const char *log_path, WorkingMemory *mem, const char *goal) {
    CriticResult result = default_critic_result();

    char *context = build_critic_context(log_path, mem, goal);
    if (!context) return result;

    /* Critic doesn't need tools_list */
    AgentCall call = { &g_agent_cfg.critic, NULL, context, 2 };
    AgentRaw raw = agent_run_call(&call);
    free(context);

    if (!raw.parsed) {
        fprintf(stderr, "[CRITIC] LLM call failed\n");
        return result;
    }

    cJSON *item;

    item = cJSON_GetObjectItem(raw.parsed, "status");
    if (cJSON_IsString(item)) {
        strncpy(result.status, item->valuestring, sizeof(result.status) - 1);
        if (strcmp(result.status, "done") != 0 &&
            strcmp(result.status, "continue") != 0 &&
            strcmp(result.status, "revise") != 0) {
            strncpy(result.status, "continue", sizeof(result.status) - 1);
        }
    }

    item = cJSON_GetObjectItem(raw.parsed, "progress");
    if (cJSON_IsNumber(item)) {
        result.progress = item->valuedouble;
        if (result.progress < 0.0) result.progress = 0.0;
        if (result.progress > 1.0) result.progress = 1.0;
    }

    item = cJSON_GetObjectItem(raw.parsed, "confidence");
    if (cJSON_IsNumber(item)) {
        result.confidence = item->valuedouble;
        if (result.confidence < 0.0) result.confidence = 0.0;
        if (result.confidence > 1.0) result.confidence = 1.0;
    }

    item = cJSON_GetObjectItem(raw.parsed, "issues");
    if (cJSON_IsArray(item)) {
        char issues_buf[1024] = {0};
        int n = cJSON_GetArraySize(item);
        for (int i = 0; i < n && i < 5; i++) {
            cJSON *issue = cJSON_GetArrayItem(item, i);
            if (cJSON_IsString(issue)) {
                if (i > 0) strncat(issues_buf, "; ", sizeof(issues_buf) - strlen(issues_buf) - 1);
                strncat(issues_buf, issue->valuestring, sizeof(issues_buf) - strlen(issues_buf) - 1);
            }
        }
        strncpy(result.issues, issues_buf, sizeof(result.issues) - 1);
    }

    item = cJSON_GetObjectItem(raw.parsed, "fix_hint");
    if (cJSON_IsString(item)) {
        strncpy(result.fix_hint, item->valuestring, sizeof(result.fix_hint) - 1);
    }

    item = cJSON_GetObjectItem(raw.parsed, "summary");
    if (cJSON_IsString(item)) {
        strncpy(result.summary, item->valuestring, sizeof(result.summary) - 1);
        strncpy(mem->summary, item->valuestring, sizeof(mem->summary) - 1);
    }

    cJSON_Delete(raw.parsed);
    free(raw.raw_json);

    fprintf(stderr, "[CRITIC] status=%s progress=%.1f confidence=%.1f\n",
            result.status, result.progress, result.confidence);

    return result;
}
