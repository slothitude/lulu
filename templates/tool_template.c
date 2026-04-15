#define _CRT_SECURE_NO_WARNINGS
#define TOOL_BUILDING_DLL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tool_api.h"
#include "tool_helpers.h"
#include "sandbox.h"

static cJSON *{{TOOL_NAME}}(cJSON *args, const char *workspace, char **error) {
    /* TODO: Extract arguments from args */
    cJSON *path_item = cJSON_GetObjectItem(args, "path");
    if (!cJSON_IsString(path_item)) {
        TOOL_ERROR("{{TOOL_NAME}} requires 'path' string argument");
    }

    /* TODO: Implement tool logic here */
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    return r;
}

static ToolInfo info = {
    "{{TOOL_NAME}}",
    "{{DESCRIPTION}}",
    "1.0",
    1,  /* requires_workspace */
    1,  /* is_idempotent */
    0   /* has_side_effects */
};

TOOL_EXPORT const ToolInfo *tool_get_info(void) { return &info; }
TOOL_EXPORT tool_execute_fn tool_get_execute(void) { return {{TOOL_NAME}}; }
