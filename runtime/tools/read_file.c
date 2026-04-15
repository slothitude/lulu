#define _CRT_SECURE_NO_WARNINGS
#define TOOL_BUILDING_DLL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tool_api.h"
#include "tool_helpers.h"
#include "sandbox.h"

static cJSON *read_file(cJSON *args, const char *workspace, char **error) {
    cJSON *path_item = cJSON_GetObjectItem(args, "path");

    if (!cJSON_IsString(path_item)) {
        *error = _strdup("read_file requires 'path' string argument");
        return NULL;
    }

    char path_buf[512];
    strncpy(path_buf, path_item->valuestring, sizeof(path_buf) - 1);
    path_buf[sizeof(path_buf) - 1] = 0;
    tool_normalize_path(path_buf);

    char *full_path = sandbox_resolve_path(path_buf, workspace);
    if (!full_path) {
        *error = _strdup("path traversal blocked or invalid path");
        return NULL;
    }

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        free(full_path);
        *error = _strdup("file not found");
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = (char *)malloc(size + 1);
    if (!content) {
        fclose(f);
        free(full_path);
        *error = _strdup("out of memory");
        return NULL;
    }

    fread(content, 1, size, f);
    content[size] = 0;
    fclose(f);
    free(full_path);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "content", content);
    free(content);
    return r;
}

static ToolInfo info = {
    TOOL_API_VERSION_MAX,
    sizeof(ToolInfo),
    "read_file",
    "Read a file from workspace",
    1,  /* requires_workspace */
    1,  /* is_idempotent */
    0   /* has_side_effects */
};

TOOL_EXPORT const ToolInfo *tool_get_info(void) { return &info; }
TOOL_EXPORT tool_execute_fn tool_get_execute(void) { return read_file; }
