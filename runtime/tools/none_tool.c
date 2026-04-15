#define _CRT_SECURE_NO_WARNINGS
#define TOOL_BUILDING_DLL
#include <stdlib.h>
#include <string.h>
#include "tool_api.h"

static cJSON *none(cJSON *args, const char *workspace, char **error) {
    (void)workspace; (void)error;

    cJSON *r = cJSON_CreateObject();
    cJSON *reason = cJSON_GetObjectItem(args, "reason");
    if (cJSON_IsString(reason)) {
        cJSON_AddStringToObject(r, "status", "skipped");
        cJSON_AddStringToObject(r, "reason", reason->valuestring);
    } else {
        cJSON_AddStringToObject(r, "status", "no_action");
    }
    return r;
}

static ToolInfo info = { "none", "Skip this step" };

TOOL_EXPORT const ToolInfo *tool_get_info(void) { return &info; }
TOOL_EXPORT tool_execute_fn tool_get_execute(void) { return none; }
