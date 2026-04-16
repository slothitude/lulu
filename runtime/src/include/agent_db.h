#pragma once

/* agent_db.h — Graph storage layer for Lulu v4.
   All state lives in an embedded RyuGraph property graph.
   No Cypher leaks outside this module — callers use C functions. */

#include <stdint.h>
#include <time.h>
#include <stdbool.h>
#include "ryu.h"
#include "cJSON.h"

/* ========================= Core ========================= */

typedef struct {
    ryu_database   _db;
    ryu_connection _conn;
    int            _initialized;
    CRITICAL_SECTION _lock;
    char           _last_error[512];
} AgentDB;

/* Open (or create) the graph database at db_path. Returns 1 on success. */
int  agent_db_open(AgentDB *adb, const char *db_path);
void agent_db_close(AgentDB *adb);

/* ========================= Thread Safety ========================= */

void agent_db_init_lock(void);
void agent_db_lock(void);
void agent_db_unlock(void);

/* ========================= Tasks ========================= */

typedef struct {
    char      id[32];
    char      name[128];
    char      prompt[4096];
    char      status[16];
    char      result[2048];
    char      source[16];
    long long chat_id;
    int       priority;
    int       attempts;
    int       max_attempts;
    char      plan[4096];
    char      state[8192];
    char      last_error[1024];
    time_t    created_at;
    time_t    updated_at;
} DbTask;

/* Create a new task. Returns malloc'd id string (caller frees). */
char  *agent_db_task_create(AgentDB *adb, const char *name,
                            const char *prompt, int priority,
                            const char *source, long long chat_id);

/* Get next runnable task as a DbTask (caller frees with agent_db_task_free). */
DbTask *agent_db_task_next(AgentDB *adb);

/* Update task fields. Pass NULL for any field to skip it. */
int     agent_db_task_update(AgentDB *adb, const char *id,
                             const char *status, const char *result,
                             const char *plan, const char *state,
                             const char *last_error);

/* Append text to task's rolling state. */
int     agent_db_task_append_state(AgentDB *adb, const char *id,
                                   const char *text);

/* Count tasks with given status (NULL = all). */
int     agent_db_task_count(AgentDB *adb, const char *status);

/* List all tasks into text buffer (for /tasks command). */
void    agent_db_task_list(AgentDB *adb, char *buf, size_t size);

/* Find task by id. Returns malloc'd DbTask or NULL. */
DbTask *agent_db_task_find(AgentDB *adb, const char *id);

/* Get all runnable candidates (pending + failed past cooldown).
   Fills out[] with malloc'd DbTask pointers. Returns count. */
int     agent_db_task_get_runnable(AgentDB *adb, DbTask **out, int max);

/* Free a DbTask. */
void    agent_db_task_free(DbTask *t);

/* ========================= Memory ========================= */

char  *agent_db_memory_add(AgentDB *adb, const char *category,
                           const char *key, const char *content);
cJSON *agent_db_memory_search(AgentDB *adb, const float *vec, int dim, int k);
void   agent_db_memory_set_scalar(AgentDB *adb, const char *key, const char *val);
char  *agent_db_memory_get_scalar(AgentDB *adb, const char *key);
void   agent_db_memory_increment(AgentDB *adb, const char *key);
int    agent_db_memory_step_count(AgentDB *adb);

/* ========================= Goals ========================= */

int agent_db_goal_create(AgentDB *adb, const char *text);

/* ========================= Sessions & Messages ========================= */

char  *agent_db_session_get_or_create(AgentDB *adb, const char *chat_id,
                                      const char *channel);
char  *agent_db_message_append(AgentDB *adb, const char *session_id,
                               const char *role, const char *content, int seq);
cJSON *agent_db_session_history(AgentDB *adb, const char *session_id);
int    agent_db_session_clear(AgentDB *adb, const char *session_id);
int    agent_db_session_prune(AgentDB *adb, int64_t ttl_seconds);

/* ========================= Tool Calls (Phase 2 — stub) ========================= */

char  *agent_db_tool_call_record(AgentDB *adb, const char *task_id,
                                 const char *tool, const char *args_json,
                                 const char *result_json, int64_t dur_ms);

/* ========================= Files ========================= */

int agent_db_file_wrote(AgentDB *adb, const char *task_id,
                        const char *path, const char *hash, int64_t size);
int agent_db_file_read(AgentDB *adb, const char *task_id, const char *path);

/* ========================= Prompt Cache ========================= */

char *agent_db_cache_get(AgentDB *adb, const char *hash);
int   agent_db_cache_set(AgentDB *adb, const char *task_id,
                         const char *hash, const char *response);

/* ========================= Scripts (Phase 4 — stub) ========================= */

char  *agent_db_script_store(AgentDB *adb, const char *name,
                             const char *desc, const char *tool_seq);
cJSON *agent_db_script_match(AgentDB *adb, const float *vec, int dim);

/* ========================= Logging ========================= */

void agent_db_log_event(AgentDB *adb, const char *type, int iter,
                        const char *stage, const char *json, int success);

/* ========================= Analytics ========================= */

cJSON *agent_db_tasks_failed_ranked(AgentDB *adb);
cJSON *agent_db_tools_unused(AgentDB *adb);
cJSON *agent_db_file_workflow(AgentDB *adb, const char *path);

/* ========================= Graph Stats ========================= */

int64_t agent_db_node_count(AgentDB *adb);
int64_t agent_db_rel_count(AgentDB *adb);

/* ========================= Migration ========================= */

int agent_db_needs_migration(AgentDB *adb);
int agent_db_migrate_tasks_json(AgentDB *adb, const char *path);
int agent_db_migrate_memory_json(AgentDB *adb, const char *path);
int agent_db_migrate_cache_json(AgentDB *adb, const char *path);
