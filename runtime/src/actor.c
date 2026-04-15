#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "actor.h"
#include "llm.h"
#include "tools.h"
#include "sandbox.h"
#include "agent_config.h"
#include "agent_core.h"

#ifdef _WIN32
#include <windows.h>
#endif

extern AgentConfig g_agent_cfg;

/* Check if a step's expected outcome is already satisfied */
static int step_already_satisfied(PlannerStep *step, const char *workspace) {
    const char *expected = step->expected;

    char *exists_str = strstr(expected, "exists");
    if (!exists_str) exists_str = strstr(expected, "file");
    if (!exists_str) return 0;

    char *p = (char *)expected;
    while (*p) {
        char *dot = strchr(p, '.');
        if (!dot) break;

        char *start = dot;
        while (start > expected && *(start - 1) != ' ' && *(start - 1) != '\'' &&
               *(start - 1) != '"' && *(start - 1) != '`') {
            start--;
        }

        char *end = dot + 1;
        while (*end && *end != ' ' && *end != '\'' && *end != '"' &&
               *end != '`' && *end != ',' && *end != ')') {
            end++;
        }

        char filename[256];
        int len = (int)(end - start);
        if (len > 0 && len < 256) {
            memcpy(filename, start, len);
            filename[len] = 0;

            char *full = sandbox_resolve_path(filename, workspace);
            if (full) {
#ifdef _WIN32
                DWORD attrs = GetFileAttributesA(full);
                free(full);
                if (attrs != INVALID_FILE_ATTRIBUTES) return 1;
#else
                struct stat st;
                int exists = (stat(full, &st) == 0);
                free(full);
                if (exists) return 1;
#endif
            }
        }
        p = end;
    }
    return 0;
}

ActorResult actor_run(PlannerStep *step, WorkingMemory *mem, const char *workspace) {
    ActorResult result = {0};
    result.args = NULL;
    result.result = NULL;
    result.success = 0;
    result.retries = 0;

    int max_attempts = g_agent_cfg.behavior.actor_max_attempts;
    if (max_attempts <= 0) max_attempts = 3;

    /* Pre-check: skip if already satisfied */
    if (step_already_satisfied(step, workspace)) {
        fprintf(stderr, "[ACTOR] Step #%d already satisfied, skipping\n", step->id);
        strncpy(result.tool, "none", sizeof(result.tool) - 1);
        result.args = cJSON_CreateObject();
        cJSON_AddStringToObject(result.args, "reason", "step already satisfied");
        result.result = cJSON_CreateObject();
        cJSON_AddStringToObject(result.result, "status", "skipped");
        result.success = 1;
        return result;
    }

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        /* Build context body */
        char *escaped_task = llm_escape_json_string(step->task);
        char *escaped_expected = llm_escape_json_string(step->expected);
        char *escaped_last = llm_escape_json_string(mem->last_result[0] ? mem->last_result : "None");
        char *escaped_tool = llm_escape_json_string(mem->last_tool[0] ? mem->last_tool : "None");

        char context[8192];
        snprintf(context, sizeof(context),
            "CURRENT STEP #%d:\n"
            "  Task: %s\n"
            "  Expected: %s\n\n"
            "PREVIOUS CONTEXT:\n"
            "  Last tool used: %s\n"
            "  Last result: %s",
            step->id,
            escaped_task,
            escaped_expected,
            escaped_tool,
            escaped_last);

        free(escaped_task); free(escaped_expected);
        free(escaped_last); free(escaped_tool);

        AgentCall call = { &g_agent_cfg.actor, g_agent_cfg.shared.tools_list, context, 2 };
        AgentRaw raw = agent_run_call(&call);

        if (!raw.parsed) {
            result.retries++;
            fprintf(stderr, "[ACTOR] Could not extract JSON (attempt %d)\n", attempt + 1);
            continue;
        }

        /* Extract tool name */
        cJSON *tool_item = cJSON_GetObjectItem(raw.parsed, "tool");
        if (!cJSON_IsString(tool_item)) {
            cJSON_Delete(raw.parsed); free(raw.raw_json);
            result.retries++;
            continue;
        }

        strncpy(result.tool, tool_item->valuestring, sizeof(result.tool) - 1);

        /* Extract arguments */
        cJSON *args = cJSON_GetObjectItem(raw.parsed, "arguments");
        if (!args) args = cJSON_GetObjectItem(raw.parsed, "args");

        if (!args) {
            args = cJSON_CreateObject();
        } else {
            args = cJSON_Duplicate(args, 1);
        }

        cJSON_Delete(raw.parsed);
        free(raw.raw_json);

        /* Normalize path arguments */
        cJSON *path_arg = cJSON_GetObjectItem(args, "path");
        if (cJSON_IsString(path_arg)) {
            char path_buf[512];
            strncpy(path_buf, path_arg->valuestring, sizeof(path_buf) - 1);
            path_buf[sizeof(path_buf) - 1] = 0;
            tools_normalize_path_arg(path_buf);
            cJSON_ReplaceItemInObject(args, "path", cJSON_CreateString(path_buf));
        }

        result.args = args;

        /* Find and execute tool */
        const LoadedTool *tool = tools_find(result.tool);
        if (!tool || !tool->enabled) {
            char errbuf[128];
            snprintf(errbuf, sizeof(errbuf), "Unknown or disabled tool: %s", result.tool);
            result.result = cJSON_CreateObject();
            cJSON_AddStringToObject(result.result, "error", errbuf);
            result.success = 0;
            break;
        }

        /* Dry-run: skip actual execution, return simulated result */
        if (g_agent_cfg.behavior.dry_run) {
            result.result = cJSON_CreateObject();
            cJSON_AddStringToObject(result.result, "status", "simulated");
            char would_do[256];
            snprintf(would_do, sizeof(would_do), "Would execute %s", result.tool);
            cJSON_AddStringToObject(result.result, "would_do", would_do);
            result.success = 1;
            fprintf(stderr, "[ACTOR] [DRY-RUN] Skipped %s\n", result.tool);
            break;
        }

        char *err = NULL;
        result.result = tool->fn(args, workspace, &err);

        if (err) {
            if (!result.result) result.result = cJSON_CreateObject();
            cJSON_AddStringToObject(result.result, "error", err);
            result.success = 0;
            fprintf(stderr, "[ACTOR] Tool error: %s\n", err);
            free(err);
        } else {
            result.success = 1;
        }

        /* Track file creation */
        if (strcmp(result.tool, "create_file") == 0 && result.success) {
            cJSON *path_item = cJSON_GetObjectItem(args, "path");
            if (cJSON_IsString(path_item)) {
                memory_track_file(mem, path_item->valuestring);
            }
        }

        break;
    }

    return result;
}
