#define _CRT_SECURE_NO_WARNINGS
#define TOOL_BUILDING_DLL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tool_api.h"

static cJSON *run_test(cJSON *args, const char *workspace, char **error) {
    (void)args;

    char test_path[512];
    snprintf(test_path, sizeof(test_path), "%s\\test_results.txt", workspace);

    FILE *f = fopen(test_path, "rb");
    if (!f) {
        *error = _strdup("no test_results.txt found");
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = (char *)malloc(size + 1);
    if (!content) {
        fclose(f);
        *error = _strdup("out of memory");
        return NULL;
    }

    fread(content, 1, size, f);
    content[size] = 0;
    fclose(f);

    cJSON *test_data = cJSON_Parse(content);
    free(content);

    if (!test_data) {
        *error = _strdup("test_results.txt is not valid JSON");
        return NULL;
    }

    return test_data;
}

static ToolInfo info = {
    TOOL_API_VERSION_MAX,
    sizeof(ToolInfo),
    "run_test",
    "Run tests from test_results.txt",
    1,  /* requires_workspace */
    1,  /* is_idempotent */
    0   /* has_side_effects */
};

TOOL_EXPORT const ToolInfo *tool_get_info(void) { return &info; }
TOOL_EXPORT tool_execute_fn tool_get_execute(void) { return run_test; }
