#pragma once
#include "state.h"

typedef struct {
    char status[16];     /* "continue" | "done" | "revise" */
    double progress;     /* 0.0 - 1.0 */
    double confidence;   /* 0.0 - 1.0 */
    char issues[1024];
    char fix_hint[512];
    char summary[512];
} CriticResult;

/* Run critic on the current log and memory. Returns evaluated result. */
CriticResult critic_run(WorkingMemory *mem, const char *goal, int iteration,
                        const char *log_path);
