#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tasks.h"
#include "agent_db.h"
#include "cJSON.h"

/* ===== Graph-backed task storage ===== */
/* All persistence is in the graph database via agent_db.
   This module provides the same API as v3 but delegates storage. */

extern AgentDB g_adb;  /* defined in main.c */

/* Small pointer cache for active tasks (preserves pointer-based API) */
static DbTask *g_cache[16];
static int g_cache_count = 0;

/* ===== Thread safety ===== */

static CRITICAL_SECTION g_tasks_lock;

void tasks_init_lock(void) { InitializeCriticalSection(&g_tasks_lock); }
void tasks_lock(void)      { EnterCriticalSection(&g_tasks_lock); }
void tasks_unlock(void)    { LeaveCriticalSection(&g_tasks_lock); }

/* ===== Load / Save (no-ops — graph is the source of truth) ===== */

void tasks_load(const char *path) {
    (void)path;  /* graph persists natively */
}

void tasks_save(void) {
    /* no-op — every mutation writes to graph immediately */
}

/* ===== Cache helpers ===== */

static void cache_clear(void) {
    for (int i = 0; i < g_cache_count; i++) {
        agent_db_task_free(g_cache[i]);
    }
    g_cache_count = 0;
}

static DbTask *cache_find(const char *id) {
    for (int i = 0; i < g_cache_count; i++) {
        if (strcmp(g_cache[i]->id, id) == 0) return g_cache[i];
    }
    return NULL;
}

static DbTask *cache_add(DbTask *t) {
    if (g_cache_count >= 16) {
        agent_db_task_free(g_cache[0]);
        memmove(&g_cache[0], &g_cache[1],
                (g_cache_count - 1) * sizeof(DbTask *));
        g_cache_count--;
    }
    g_cache[g_cache_count++] = t;
    return t;
}

/* ===== CRUD ===== */

Task *tasks_create(const char *prompt, const char *source,
                   long long chat_id, int priority) {
    char *id = agent_db_task_create(&g_adb, "", prompt, priority,
                                    source, chat_id);
    if (!id) return NULL;

    /* Fetch back from graph to get a cached pointer */
    DbTask *dbt = agent_db_task_find(&g_adb, id);
    free(id);
    if (!dbt) return NULL;

    return (Task *)cache_add(dbt);
}

Task *tasks_next_runnable(void) {
    cache_clear();
    DbTask *dbt = agent_db_task_next(&g_adb);
    if (!dbt) return NULL;
    return (Task *)cache_add(dbt);
}

void tasks_update(Task *t, const char *status, const char *result) {
    if (!t) return;
    DbTask *dbt = (DbTask *)t;
    agent_db_task_update(&g_adb, dbt->id, status, result,
                         NULL, NULL, NULL);
    /* Update in-memory copy */
    strncpy(dbt->status, status, TASK_STATUS_MAX - 1);
    if (result) strncpy(dbt->result, result, TASK_RESULT_MAX - 1);
    dbt->updated_at = time(NULL);
}

void tasks_append_state(Task *t, const char *text) {
    if (!t || !text) return;
    DbTask *dbt = (DbTask *)t;
    agent_db_task_append_state(&g_adb, dbt->id, text);

    /* Update in-memory copy */
    size_t cur = strlen(dbt->state);
    size_t add = strlen(text);
    if (cur + add + 2 >= TASK_STATE_MAX) {
        size_t keep = TASK_STATE_MAX - add - 2;
        if (keep < cur) {
            memmove(dbt->state, dbt->state + (cur - keep), keep);
            dbt->state[keep] = 0;
        }
    }
    if (dbt->state[0]) strcat(dbt->state, "\n");
    strncat(dbt->state, text, TASK_STATE_MAX - strlen(dbt->state) - 1);
}

int tasks_count(const char *status) {
    return agent_db_task_count(&g_adb, status);
}

Task *tasks_find(const char *id) {
    /* Check cache first */
    DbTask *cached = cache_find(id);
    if (cached) return (Task *)cached;

    DbTask *dbt = agent_db_task_find(&g_adb, id);
    if (!dbt) return NULL;
    return (Task *)cache_add(dbt);
}

void tasks_list(char *buf, size_t buf_size) {
    agent_db_task_list(&g_adb, buf, buf_size);
}

int tasks_get_runnable(Task **out, int max) {
    /* Allocate DbTask pointers, cast to Task* */
    DbTask **dbout = (DbTask **)out;
    int n = agent_db_task_get_runnable(&g_adb, dbout, max);

    /* Cache them */
    for (int i = 0; i < n; i++) {
        cache_add(dbout[i]);
    }
    return n;
}
