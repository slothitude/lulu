#pragma once
#include "cJSON.h"
#include "state.h"
#include "planner.h"

/* Actor result */
typedef struct {
    char tool[64];
    cJSON *args;
    cJSON *result;
    int  success;
    int  retries;
} ActorResult;

/* Run actor for a step. Returns result with allocated cJSON objects. */
ActorResult actor_run(PlannerStep *step, WorkingMemory *mem, const char *workspace,
                      int iteration);
