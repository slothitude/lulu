#pragma once

/* Returns 1 if path is safe (no traversal, within workspace), 0 otherwise */
int sandbox_is_path_safe(const char *path, const char *workspace);

/* Resolves path within workspace. Returns malloc'd absolute path or NULL. */
char *sandbox_resolve_path(const char *path, const char *workspace);
