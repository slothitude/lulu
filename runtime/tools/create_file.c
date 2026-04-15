#define _CRT_SECURE_NO_WARNINGS
#define TOOL_BUILDING_DLL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tool_api.h"
#include "tool_helpers.h"
#include "sandbox.h"

static cJSON *create_file(cJSON *args, const char *workspace, char **error) {
    cJSON *path_item = cJSON_GetObjectItem(args, "path");
    cJSON *content_item = cJSON_GetObjectItem(args, "content");

    if (!cJSON_IsString(path_item) || !cJSON_IsString(content_item)) {
        *error = _strdup("create_file requires 'path' and 'content' string arguments");
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

    /* Check if file already exists with same content */
    FILE *check = fopen(full_path, "rb");
    if (check) {
        fseek(check, 0, SEEK_END);
        long existing_size = ftell(check);
        fseek(check, 0, SEEK_SET);
        char *existing = (char *)malloc(existing_size + 1);
        if (existing) {
            fread(existing, 1, existing_size, check);
            existing[existing_size] = 0;
        }
        fclose(check);

        const char *new_content = content_item->valuestring;
        if (existing && strcmp(existing, new_content) == 0) {
            free(existing);
            free(full_path);
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "status", "exists_same");
            return r;
        }
        free(existing);
    }

    /* Create parent directories */
    tool_mkdirs(full_path);

    FILE *f = fopen(full_path, "wb");
    if (!f) {
        free(full_path);
        *error = _strdup("failed to create file");
        return NULL;
    }

    const char *content = content_item->valuestring;
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    free(full_path);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", check ? "overwritten" : "created");
    return r;
}

static ToolInfo info = {
    "create_file",
    "Create a file with given content",
    "1.0",
    1,  /* requires_workspace */
    0,  /* is_idempotent */
    1   /* has_side_effects */
};

TOOL_EXPORT const ToolInfo *tool_get_info(void) { return &info; }
TOOL_EXPORT tool_execute_fn tool_get_execute(void) { return create_file; }
