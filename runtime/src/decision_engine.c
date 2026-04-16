#define _CRT_SECURE_NO_WARNINGS
#include "decision_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ========================= Config ========================= */

static DecisionConfig g_cfg = {
    .epsilon = 0.1f,
    .max_candidates = 10
};

static char g_last_debug[1024];

/* ========================= Utils ========================= */

static float randf(void) {
    return (float)rand() / (float)RAND_MAX;
}

static float age_bonus(const Task *t) {
    time_t now = time(NULL);
    float age_sec = (float)difftime(now, t->created_at);
    /* Caps at 5.0 after ~5 minutes */
    return (float)fmin(age_sec / 60.0, 5.0);
}

static float failure_penalty(const Task *t) {
    return (float)t->attempts * 1.5f;
}

/* ========================= Scoring ========================= */

static float score_task(const Task *t) {
    float score = 0.0f;

    /* Priority: direct weight */
    score += 2.0f * (float)t->priority;

    /* Age: tasks waiting longer get a boost */
    score += 1.5f * age_bonus(t);

    /* Failure: penalize repeated attempts */
    score -= 2.0f * failure_penalty(t);

    /* Failed tasks that exhausted retries get heavy penalty */
    if (t->attempts >= t->max_attempts && strcmp(t->status, "failed") == 0)
        score -= 10.0f;

    return score;
}

/* ========================= Main Decision ========================= */

int decision_pick_task(char *out_task_id) {
    Task *candidates[16] = {0};
    int n;

    /* Fetch runnable candidates under tasks lock */
    tasks_lock();
    n = tasks_get_runnable(candidates,
        g_cfg.max_candidates > 16 ? 16 : g_cfg.max_candidates);
    tasks_unlock();

    if (n == 0) return 0;

    /* Exploration: pick random candidate */
    if (randf() < g_cfg.epsilon) {
        int idx = rand() % n;
        strncpy(out_task_id, candidates[idx]->id, TASK_ID_MAX - 1);
        out_task_id[TASK_ID_MAX - 1] = 0;
        snprintf(g_last_debug, sizeof(g_last_debug),
                 "explore -> %s (1 of %d)", out_task_id, n);
        return 1;
    }

    /* Exploitation: score all candidates, pick best */
    float best_score = -1e9f;
    int best_idx = 0;

    for (int i = 0; i < n; i++) {
        float s = score_task(candidates[i]);
        if (s > best_score) {
            best_score = s;
            best_idx = i;
        }
    }

    strncpy(out_task_id, candidates[best_idx]->id, TASK_ID_MAX - 1);
    out_task_id[TASK_ID_MAX - 1] = 0;

    snprintf(g_last_debug, sizeof(g_last_debug),
             "exploit -> %s (score=%.2f, %d candidates)",
             out_task_id, best_score, n);

    return 1;
}

/* ========================= Learning ========================= */

void decision_learn(const char *task_id, int success) {
    /* Phase 1: log only. Later phases: adjust weights, reinforce scripts. */
    snprintf(g_last_debug, sizeof(g_last_debug),
             "learn task=%s success=%d", task_id, success);
}

/* ========================= Init ========================= */

void decision_init(DecisionConfig cfg) {
    g_cfg = cfg;
    srand((unsigned int)time(NULL));
    g_last_debug[0] = 0;
}

/* ========================= Debug ========================= */

void decision_debug_last(char *buf, size_t size) {
    strncpy(buf, g_last_debug, size - 1);
    buf[size - 1] = 0;
}
