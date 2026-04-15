#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "tools.h"

#ifdef _WIN32
#include <windows.h>
#endif

static LoadedTool g_tools[MAX_LOADED_TOOLS];
static int g_tool_count = 0;

/* ========================= Argument Normalization ========================= */

void tools_normalize_path_arg(char *path) {
    if (!path) return;
    int len = (int)strlen(path);
    int start = 0;
    while (start < len && isspace((unsigned char)path[start])) start++;
    int end = len - 1;
    while (end > start && isspace((unsigned char)path[end])) end--;
    path[end + 1] = 0;
    if (start > 0) memmove(path, path + start, end - start + 2);

    char *p = path;
    while (*p == '.' && (*(p + 1) == '/' || *(p + 1) == '\\')) p += 2;
    if (p != path) memmove(path, p, strlen(p) + 1);
}

/* ========================= DLL Loader ========================= */

int tools_load_all(const char *dir) {
    g_tool_count = 0;

#ifdef _WIN32
    char search[MAX_PATH];
    snprintf(search, sizeof(search), "%s\\*.dll", dir);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[TOOLS] No DLLs found in %s\n", dir);
        return 0;
    }

    do {
        if (g_tool_count >= MAX_LOADED_TOOLS) break;

        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);

        HMODULE lib = LoadLibraryA(path);
        if (!lib) {
            fprintf(stderr, "[TOOLS] Failed to load %s\n", fd.cFileName);
            continue;
        }

        const ToolInfo *(*get_info)(void) =
            (const ToolInfo *(*)(void))(void *)GetProcAddress(lib, "tool_get_info");
        tool_execute_fn (*get_exec)(void) =
            (tool_execute_fn (*)(void))(void *)GetProcAddress(lib, "tool_get_execute");

        if (!get_info || !get_exec) {
            fprintf(stderr, "[TOOLS] %s: missing exports\n", fd.cFileName);
            FreeLibrary(lib);
            continue;
        }

        const ToolInfo *info = get_info();
        tool_execute_fn fn = get_exec();

        if (!info || !fn || !info->name) {
            fprintf(stderr, "[TOOLS] %s: invalid info/fn\n", fd.cFileName);
            FreeLibrary(lib);
            continue;
        }

        LoadedTool *t = &g_tools[g_tool_count];
        strncpy(t->name, info->name, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = 0;
        t->fn = fn;
        t->enabled = 1;
        t->handle = (void *)lib;
        t->requires_workspace = info->requires_workspace;
        t->is_idempotent = info->is_idempotent;
        t->has_side_effects = info->has_side_effects;
        if (info->description)
            strncpy(t->description, info->description, sizeof(t->description) - 1);
        g_tool_count++;

        fprintf(stderr, "[TOOLS] Loaded: %s\n", info->name);
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
#endif

    return g_tool_count;
}

void tools_set_enabled(const char *name, int enabled) {
    for (int i = 0; i < g_tool_count; i++) {
        if (strcmp(g_tools[i].name, name) == 0) {
            g_tools[i].enabled = enabled;
            return;
        }
    }
}

const LoadedTool *tools_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_tool_count; i++) {
        if (strcmp(g_tools[i].name, name) == 0)
            return &g_tools[i];
    }
    return NULL;
}

char *tools_get_enabled_names(void) {
    /* Calculate needed size */
    size_t len = 1;
    for (int i = 0; i < g_tool_count; i++) {
        if (g_tools[i].enabled) {
            len += strlen(g_tools[i].name) + 1; /* name + ';' */
        }
    }
    char *buf = (char *)malloc(len);
    if (!buf) return NULL;
    buf[0] = 0;
    for (int i = 0; i < g_tool_count; i++) {
        if (g_tools[i].enabled) {
            if (buf[0]) strcat(buf, ";");
            strcat(buf, g_tools[i].name);
        }
    }
    return buf;
}

void tools_cleanup(void) {
#ifdef _WIN32
    for (int i = 0; i < g_tool_count; i++) {
        if (g_tools[i].handle) {
            FreeLibrary((HMODULE)g_tools[i].handle);
            g_tools[i].handle = NULL;
        }
    }
#endif
    g_tool_count = 0;
}
