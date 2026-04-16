#pragma once

#include <time.h>

/* Persistent task system — tasks survive restarts.
   Richer than a simple queue: rolling state, error tracking, cached plans. */

#define TASK_ID_MAX     32
#define TASK_PROMPT_MAX 4096
#define TASK_RESULT_MAX 2048
#define TASK_STATE_MAX  4096
#define TASK_ERROR_MAX  1024
#define TASK_PLAN_MAX   4096
#define TASK_SOURCE_MAX 16
#define TASK_STATUS_MAX 16
#define MAX_TASKS       128

typedef struct {
    char id[TASK_ID_MAX];           /* "task_1" */
    char status[TASK_STATUS_MAX];   /* "pending", "running", "done", "failed" */
    int  priority;                  /* higher = more urgent */
    char source[TASK_SOURCE_MAX];   /* "telegram", "cli" */
    long long chat_id;              /* origin channel */
    char prompt[TASK_PROMPT_MAX];   /* original goal/task description */
    char result[TASK_RESULT_MAX];   /* final output */
    char state[TASK_STATE_MAX];     /* rolling context / notes from execution */
    char last_error[TASK_ERROR_MAX];/* most recent error */
    char plan[TASK_PLAN_MAX];       /* cached plan from planning phase */
    int  attempts;                  /* how many times we've tried */
    int  max_attempts;              /* ceiling (default 3) */
    time_t created_at;
    time_t updated_at;
} Task;

/* Load tasks from JSON file. */
void tasks_load(const char *path);

/* Save tasks to JSON file (atomic write). */
void tasks_save(void);

/* Create a new task. Returns pointer to task (internal storage). */
Task *tasks_create(const char *prompt, const char *source, long long chat_id, int priority);

/* Get next runnable task using priority + cooldown scheduling.
   Returns NULL if nothing is eligible. */
Task *tasks_next_runnable(void);

/* Update task status and result. Also updates updated_at. */
void tasks_update(Task *t, const char *status, const char *result);

/* Append to task's rolling state field. */
void tasks_append_state(Task *t, const char *text);

/* Get count of tasks with given status (or total if status is NULL). */
int  tasks_count(const char *status);

/* Find task by id. Returns NULL if not found. */
Task *tasks_find(const char *id);

/* List all tasks (for /tasks command). Prints to supplied buffer. */
void tasks_list(char *buf, size_t buf_size);

/* Thread safety — initialize before multi-threaded access */
void tasks_init_lock(void);
void tasks_lock(void);
void tasks_unlock(void);
