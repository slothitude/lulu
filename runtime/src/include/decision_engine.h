#pragma once

#include "tasks.h"

/* Decision engine — replaces FIFO scheduling with scored task selection.
   Uses priority, age, failure history. Exploration/exploitation via epsilon.
   Stateless core, no graph dependency in Phase 1. */

typedef struct {
    float epsilon;          /* exploration rate (0.0 - 1.0) */
    int   max_candidates;   /* how many tasks to evaluate per pick */
} DecisionConfig;

/* Init with config. Call once at startup. */
void decision_init(DecisionConfig cfg);

/* Pick best task from runnable candidates.
   Returns 1 and fills out_task_id if a task was selected, 0 if none. */
int decision_pick_task(char *out_task_id);

/* Learning hook — call after task execution completes.
   Adjusts internal state based on outcome. */
void decision_learn(const char *task_id, int success);

/* Fill buf with debug info about the last decision. */
void decision_debug_last(char *buf, size_t size);
