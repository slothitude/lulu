#define _CRT_SECURE_NO_WARNINGS
#define TOOL_BUILDING_DLL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tool_api.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#define MAX_DEPTH 16

static void list_recursive(const char *dir, cJSON *arr, int depth) {
    if (depth > MAX_DEPTH) return;

#ifdef _WIN32
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;

        cJSON_AddItemToArray(arr, cJSON_CreateString(fd.cFileName));

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char full[MAX_PATH];
            snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
            list_recursive(full, arr, depth + 1);
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        cJSON_AddItemToArray(arr, cJSON_CreateString(ent->d_name));
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
            list_recursive(full, arr, depth + 1);
    }
    closedir(d);
#endif
}

static cJSON *list_files(cJSON *args, const char *workspace, char **error) {
    (void)args; (void)error;
    cJSON *r = cJSON_CreateObject();
    cJSON *files = cJSON_CreateArray();
    list_recursive(workspace, files, 0);
    cJSON_AddItemToObject(r, "files", files);
    return r;
}

static ToolInfo info = { "list_files", "List all files in workspace" };

TOOL_EXPORT const ToolInfo *tool_get_info(void) { return &info; }
TOOL_EXPORT tool_execute_fn tool_get_execute(void) { return list_files; }
