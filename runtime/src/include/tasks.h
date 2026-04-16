#pragma once

#include <time.h>
#include "agent_db.h"

/* Persistent task system — graph-backed in v4.
   Task is now an alias for DbTask to preserve the pointer-based API. */

#define TASK_ID_MAX     32
#define TASK_PROMPT_MAX 4096
#define TASK_RESULT_MAX 2048
#define TASK_STATE_MAX  8192
#define TASK_ERROR_MAX  1024
#define TASK_PLAN_MAX   4096
#define TASK_SOURCE_MAX 16
#define TASK_STATUS_MAX 16

/* Task is DbTask from agent_db.h — same memory layout, same fields. */
typedef DbTask Task;

/* Load tasks (no-op in v4 — graph is source of truth). */
void tasks_load(const char *path);

/* Save tasks (no-op in v4 — every mutation writes to graph immediately). */
void tasks_save(void);

/* Create a new task. Returns pointer to task (internal cache). */
Task *tasks_create(const char *prompt, const char *source, long long chat_id, int priority);

/* Get next runnable task using priority + cooldown scheduling. */
Task *tasks_next_runnable(void);

/* Update task status and result. */
void tasks_update(Task *t, const char *status, const char *result);

/* Append to task's rolling state field. */
void tasks_append_state(Task *t, const char *text);

/* Get count of tasks with given status (or total if status is NULL). */
int  tasks_count(const char *status);

/* Find task by id. Returns NULL if not found. */
Task *tasks_find(const char *id);

/* List all tasks (for /tasks command). Prints to supplied buffer. */
void tasks_list(char *buf, size_t buf_size);

/* Get runnable task candidates (pending + failed past cooldown). */
int tasks_get_runnable(Task **out, int max);

/* Thread safety */
void tasks_init_lock(void);
void tasks_lock(void);
void tasks_unlock(void);
