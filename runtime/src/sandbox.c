#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include "sandbox.h"

#ifdef _WIN32
#include <windows.h>
#define PATH_SEP '\\'
#define PATH_SEP_STR "\\"
#else
#include <unistd.h>
#include <libgen.h>
#define PATH_SEP '/'
#define PATH_SEP_STR "/"
#endif

int sandbox_is_path_safe(const char *path, const char *workspace) {
    if (!path || !workspace) return 0;

    /* Block empty paths */
    if (path[0] == '\0') return 0;

    /* Block absolute paths */
    if (path[0] == '/' || path[0] == '\\' ||
        (isalpha((unsigned char)path[0]) && path[1] == ':')) {
        return 0;
    }

    /* Block path traversal */
    if (strstr(path, "..") != NULL) return 0;

    /* Block common dangerous locations */
    if (strstr(path, "/etc/") || strstr(path, "\\etc\\")) return 0;
    if (strstr(path, "/usr/") || strstr(path, "\\usr\\")) return 0;
    if (strstr(path, "/var/") || strstr(path, "\\var\\")) return 0;
    if (strstr(path, "/tmp/")  || strstr(path, "\\tmp\\"))  return 0;
    if (strstr(path, "/dev/")  || strstr(path, "\\dev\\"))  return 0;
    if (strstr(path, "/proc/") || strstr(path, "\\proc\\")) return 0;
    if (strstr(path, "system32") || strstr(path, "System32")) return 0;
    if (strstr(path, "windows") || strstr(path, "Windows")) return 0;

    return 1;
}

char *sandbox_resolve_path(const char *path, const char *workspace) {
    if (!sandbox_is_path_safe(path, workspace)) return NULL;

    /* Normalize slashes */
    char normalized[512];
    strncpy(normalized, path, sizeof(normalized) - 1);
    normalized[sizeof(normalized) - 1] = 0;

    /* Remove leading ./ */
    char *p = normalized;
    while (*p == '.' && (*(p + 1) == '/' || *(p + 1) == '\\')) {
        p += 2;
    }

    /* Convert all slashes to platform separator */
    for (char *s = p; *s; s++) {
        if (*s == '/' || *s == '\\') *s = PATH_SEP;
    }

    /* Build full path */
    size_t wlen = strlen(workspace);
    size_t plen = strlen(p);
    char *full = (char *)malloc(wlen + 1 + plen + 1);
    if (!full) return NULL;

    snprintf(full, wlen + 1 + plen + 1, "%s%s%s", workspace, PATH_SEP_STR, p);

#ifdef _WIN32
    /* Post-canonicalization: resolve to full path and verify prefix */
    char resolved[MAX_PATH];
    if (GetFullPathNameA(full, MAX_PATH, resolved, NULL) == 0) {
        free(full);
        return NULL;
    }

    /* Verify the resolved path still starts with workspace */
    char resolved_workspace[MAX_PATH];
    if (GetFullPathNameA(workspace, MAX_PATH, resolved_workspace, NULL) == 0) {
        free(full);
        return NULL;
    }

    if (strncmp(resolved, resolved_workspace, strlen(resolved_workspace)) != 0) {
        free(full);
        return NULL;
    }

    free(full);
    return _strdup(resolved);
#else
    /* POSIX: use realpath */
    char resolved[PATH_MAX];
    if (!realpath(full, resolved)) {
        /* File doesn't exist yet — verify parent */
        free(full);
        return NULL;
    }
    free(full);
    return strdup(resolved);
#endif
}
