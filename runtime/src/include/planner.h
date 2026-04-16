#pragma once
#include "cJSON.h"
#include "state.h"

/* Planner step structure */
typedef struct {
    int  id;
    char task[256];
    char expected[512];
} PlannerStep;

/* Run planner, returns array of steps. Sets *count. Returns NULL on failure. */
PlannerStep *planner_run(const char *goal, WorkingMemory *mem, const char *extra_hint,
                         int *count, int iteration);
