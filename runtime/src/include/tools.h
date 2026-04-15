#pragma once
#include "tool_api.h"

typedef struct {
    char name[64];
    tool_execute_fn fn;
    int enabled;
    void *handle;       /* platform: HMODULE on Win, void* on POSIX */
} LoadedTool;

#define MAX_LOADED_TOOLS 32

/* Load all tool DLLs from directory. Returns count loaded. */
int tools_load_all(const char *dir);

/* Set a tool enabled/disabled by name */
void tools_set_enabled(const char *name, int enabled);

/* Find tool by name. Returns NULL if not found. Check ->enabled before use. */
const LoadedTool *tools_find(const char *name);

/* Normalize path arguments: trim whitespace, normalize leading ./ */
void tools_normalize_path_arg(char *path);

/* Cleanup: free all loaded DLLs */
void tools_cleanup(void);
