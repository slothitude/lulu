#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "planner.h"
#include "llm.h"
#include "state.h"
#include "agent_config.h"
#include "agent_core.h"

extern AgentConfig g_agent_cfg;

PlannerStep *planner_run(const char *goal, WorkingMemory *mem, const char *extra_hint, int *count) {
    *count = 0;

    /* Build context body */
    char *escaped_goal = llm_escape_json_string(goal);
    char *escaped_summary = llm_escape_json_string(mem->summary[0] ? mem->summary : "No previous work done yet.");
    char *escaped_hint = extra_hint ? llm_escape_json_string(extra_hint) : NULL;

    char context[8192];
    snprintf(context, sizeof(context),
        "GOAL: %s\n\nPREVIOUS WORK: %s\n%s%s%s",
        escaped_goal, escaped_summary,
        extra_hint ? "\nADDITIONAL CONTEXT: " : "",
        extra_hint ? escaped_hint : "",
        extra_hint ? "\n" : "");
    free(escaped_goal); free(escaped_summary); free(escaped_hint);

    /* Call generic engine */
    AgentCall call = { &g_agent_cfg.planner, g_agent_cfg.shared.tools_list, context, 3 };
    AgentRaw raw = agent_run_call(&call);
    if (!raw.parsed) {
        fprintf(stderr, "[PLANNER] LLM call failed\n");
        return NULL;
    }

    /* Parse role-specific output: steps[] */
    cJSON *steps_arr = cJSON_GetObjectItem(raw.parsed, "steps");
    if (!cJSON_IsArray(steps_arr)) {
        cJSON_Delete(raw.parsed); free(raw.raw_json);
        return NULL;
    }

    int n = cJSON_GetArraySize(steps_arr);
    if (n <= 0 || n > 10) {
        cJSON_Delete(raw.parsed); free(raw.raw_json);
        return NULL;
    }

    PlannerStep *steps = (PlannerStep *)calloc(n, sizeof(PlannerStep));
    if (!steps) {
        cJSON_Delete(raw.parsed); free(raw.raw_json);
        return NULL;
    }

    for (int i = 0; i < n; i++) {
        cJSON *step_obj = cJSON_GetArrayItem(steps_arr, i);

        cJSON *id_item = cJSON_GetObjectItem(step_obj, "id");
        steps[i].id = cJSON_IsNumber(id_item) ? id_item->valueint : (i + 1);

        cJSON *task_item = cJSON_GetObjectItem(step_obj, "task");
        if (cJSON_IsString(task_item)) {
            strncpy(steps[i].task, task_item->valuestring, sizeof(steps[i].task) - 1);
        }

        cJSON *expected_item = cJSON_GetObjectItem(step_obj, "expected");
        if (cJSON_IsString(expected_item)) {
            strncpy(steps[i].expected, expected_item->valuestring, sizeof(steps[i].expected) - 1);
        }
    }

    *count = n;
    cJSON_Delete(raw.parsed);
    free(raw.raw_json);

    fprintf(stderr, "[PLANNER] Generated %d steps\n", n);
    return steps;
}
