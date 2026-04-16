#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mem_subscriber.h"
#include "event_bus.h"

static WorkingMemory *g_mem = NULL;
static const char    *g_memory_path = NULL;

static int on_tool_result(const Event *ev, void *ud) {
    (void)ud;

    /* Update memory with tool result */
    char *result_str = ev->tool_result.result
        ? cJSON_PrintUnformatted((cJSON *)ev->tool_result.result) : NULL;
    memory_update(g_mem, ev->tool_result.tool, result_str, ev->tool_result.success);
    free(result_str);

    /* Track file creation */
    if (ev->tool_result.success && strcmp(ev->tool_result.tool, "create_file") == 0
        && ev->tool_result.args) {
        cJSON *path_item = cJSON_GetObjectItem((cJSON *)ev->tool_result.args, "path");
        if (cJSON_IsString(path_item)) {
            memory_track_file(g_mem, path_item->valuestring);
        }
    }

    memory_save(g_mem, g_memory_path);
    return 0;
}

static int on_state_update(const Event *ev, void *ud) {
    (void)ud;

    /* Update summary from critic */
    if (ev->state_update.summary && ev->state_update.summary[0]) {
        strncpy(g_mem->summary, ev->state_update.summary, sizeof(g_mem->summary) - 1);
    }

    memory_save(g_mem, g_memory_path);
    return 0;
}

void mem_subscriber_init(WorkingMemory *mem, const char *memory_path) {
    g_mem = mem;
    g_memory_path = memory_path;
    event_bus_subscribe(EVENT_TOOL_RESULT,  on_tool_result,  NULL);
    event_bus_subscribe(EVENT_STATE_UPDATE, on_state_update, NULL);
}
