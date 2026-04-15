#ifndef TOOL_API_H
#define TOOL_API_H

#include "cJSON.h"

#define TOOL_API_VERSION_MIN 1
#define TOOL_API_VERSION_MAX 1

#ifdef _WIN32
  #ifdef TOOL_BUILDING_DLL
    #define TOOL_EXPORT __declspec(dllexport)
  #else
    #define TOOL_EXPORT
  #endif
#else
  #define TOOL_EXPORT __attribute__((visibility("default")))
#endif

typedef struct {
    int api_version;               /* must be in [TOOL_API_VERSION_MIN, TOOL_API_VERSION_MAX] */
    int struct_size;               /* must equal sizeof(ToolInfo) */
    const char *name;
    const char *description;
    int requires_workspace;
    int is_idempotent;             /* safe to retry */
    int has_side_effects;
} ToolInfo;

typedef cJSON *(*tool_execute_fn)(cJSON *args, const char *workspace, char **error);

TOOL_EXPORT const ToolInfo *tool_get_info(void);
TOOL_EXPORT tool_execute_fn tool_get_execute(void);

#endif
