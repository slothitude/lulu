#ifndef TOOL_HELPERS_H
#define TOOL_HELPERS_H

#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

/* Normalize path: trim whitespace, strip leading ./ */
static inline void tool_normalize_path(char *path) {
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

/* Create parent directories for a file path */
static inline int tool_mkdirs(const char *path) {
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    char *last_sep = strrchr(tmp, '/');
    char *last_sep2 = strrchr(tmp, '\\');
    char *sep = (last_sep2 > last_sep) ? last_sep2 : last_sep;
    if (!sep) return 0;
    *sep = 0;
    if (tmp[0] == 0) return 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = 0;
            MKDIR(tmp);
            *p = '/';
        }
    }
    MKDIR(tmp);
    return 1;
}

#endif
