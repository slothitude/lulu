#define _CRT_SECURE_NO_WARNINGS
#include "decision_engine.h"
#include "agent_db.h"
#include "cJSON.h"
#include "ryu.h"
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

/* ========================= Learned Weights ========================= */

/* In-memory cache of decision weights from graph.
   Keyed by tool_sequence string, value is a weight multiplier. */
#define MAX_WEIGHTS 64
typedef struct {
    char tool_seq[256];  /* e.g. "create_file,read_file" */
    float weight;        /* multiplier: >1 = good, <1 = bad */
    int samples;         /* number of observations */
    time_t last_updated;
} DecisionWeight;

static DecisionWeight g_weights[MAX_WEIGHTS];
static int g_weights_count = 0;
static time_t g_weights_loaded = 0;

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

/* ========================= Weight Management ========================= */

static DecisionWeight *find_weight(const char *seq) {
    for (int i = 0; i < g_weights_count; i++) {
        if (strcmp(g_weights[i].tool_seq, seq) == 0)
            return &g_weights[i];
    }
    return NULL;
}

static DecisionWeight *add_weight(const char *seq) {
    if (g_weights_count >= MAX_WEIGHTS) {
        /* Evict oldest */
        int oldest = 0;
        for (int i = 1; i < g_weights_count; i++) {
            if (g_weights[i].last_updated < g_weights[oldest].last_updated)
                oldest = i;
        }
        g_weights[oldest] = g_weights[--g_weights_count];
    }
    DecisionWeight *w = &g_weights[g_weights_count++];
    strncpy(w->tool_seq, seq, sizeof(w->tool_seq) - 1);
    w->tool_seq[sizeof(w->tool_seq) - 1] = 0;
    w->weight = 1.0f;
    w->samples = 0;
    w->last_updated = time(NULL);
    return w;
}

/* Load weights from graph MEMORY nodes */
static void load_weights_from_graph(void) {
    extern AgentDB g_adb;
    agent_db_lock();

    ryu_query_result result;
    const char *q = "MATCH (m:MEMORY) WHERE m.category = 'decision_weights' "
                    "RETURN m.key, m.content";
    ryu_state st = ryu_connection_query(&g_adb._conn, q, &result);
    if (st == RyuSuccess && ryu_query_result_is_success(&result)) {
        g_weights_count = 0;
        while (ryu_query_result_has_next(&result) && g_weights_count < MAX_WEIGHTS) {
            ryu_flat_tuple tup;
            ryu_query_result_get_next(&result, &tup);

            char *key_str = NULL, *content_str = NULL;
            ryu_value v0, v1;
            ryu_flat_tuple_get_value(&tup, 0, &v0);
            ryu_flat_tuple_get_value(&tup, 1, &v1);
            ryu_value_get_string(&v0, &key_str);
            ryu_value_get_string(&v1, &content_str);

            if (key_str && content_str) {
                cJSON *j = cJSON_Parse(content_str);
                if (j) {
                    DecisionWeight *w = &g_weights[g_weights_count];
                    strncpy(w->tool_seq, key_str, sizeof(w->tool_seq) - 1);
                    w->tool_seq[sizeof(w->tool_seq) - 1] = 0;
                    cJSON *wj = cJSON_GetObjectItem(j, "weight");
                    cJSON *ws = cJSON_GetObjectItem(j, "samples");
                    w->weight = wj ? (float)wj->valuedouble : 1.0f;
                    w->samples = ws ? ws->valueint : 0;
                    w->last_updated = time(NULL);
                    g_weights_count++;
                    cJSON_Delete(j);
                }
            }
            if (key_str) ryu_destroy_string(key_str);
            if (content_str) ryu_destroy_string(content_str);
            ryu_value_destroy(&v0);
            ryu_value_destroy(&v1);
            ryu_flat_tuple_destroy(&tup);
        }
    }
    ryu_query_result_destroy(&result);
    agent_db_unlock();
    g_weights_loaded = time(NULL);
}

/* Persist a weight back to graph */
static void save_weight_to_graph(const DecisionWeight *w) {
    extern AgentDB g_adb;
    char value[512];
    snprintf(value, sizeof(value),
        "{\"tool_seq\":\"%s\",\"weight\":%.4f,\"samples\":%d}",
        w->tool_seq, w->weight, w->samples);

    agent_db_lock();
    agent_db_memory_add(&g_adb, "decision_weights", w->tool_seq, value);
    agent_db_unlock();
}

/* Extract tool sequence from a task's TOOL_CALL records */
static char *get_task_tool_sequence(const char *task_id) {
    extern AgentDB g_adb;
    char *result = NULL;

    agent_db_lock();
    char q[512];
    snprintf(q, sizeof(q),
        "MATCH (t:TASK)-[:CALLED]->(tc:TOOL_CALL) WHERE t.id = '%s' "
        "RETURN tc.tool_name ORDER BY tc.id", task_id);

    ryu_query_result qr;
    ryu_state st = ryu_connection_query(&g_adb._conn, q, &qr);
    if (st == RyuSuccess && ryu_query_result_is_success(&qr)) {
        /* Build comma-separated tool sequence */
        char buf[256];
        int pos = 0;
        while (ryu_query_result_has_next(&qr)) {
            ryu_flat_tuple tup;
            ryu_query_result_get_next(&qr, &tup);
            char *name = NULL;
            ryu_value v;
            ryu_flat_tuple_get_value(&tup, 0, &v);
            ryu_value_get_string(&v, &name);
            if (name) {
                if (pos > 0) buf[pos++] = ',';
                int nlen = (int)strlen(name);
                if (pos + nlen < (int)sizeof(buf) - 1) {
                    memcpy(buf + pos, name, nlen);
                    pos += nlen;
                }
                ryu_destroy_string(name);
            }
            ryu_value_destroy(&v);
            ryu_flat_tuple_destroy(&tup);
        }
        buf[pos] = 0;
        if (pos > 0) result = _strdup(buf);
    }
    ryu_query_result_destroy(&qr);
    agent_db_unlock();

    return result;
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

    /* Reload weights from graph periodically (every 60s) */
    if (difftime(time(NULL), g_weights_loaded) > 60.0) {
        load_weights_from_graph();
    }

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
    if (!task_id) return;

    /* Reload weights if stale */
    if (g_weights_loaded == 0 || difftime(time(NULL), g_weights_loaded) > 60.0) {
        load_weights_from_graph();
    }

    /* Get tool sequence for this task */
    char *seq = get_task_tool_sequence(task_id);
    if (!seq || !seq[0]) {
        snprintf(g_last_debug, sizeof(g_last_debug),
                 "learn task=%s success=%d (no tool sequence)", task_id, success);
        free(seq);
        return;
    }

    DecisionWeight *w = find_weight(seq);
    if (!w) w = add_weight(seq);

    /* Update weight using exponential moving average */
    float alpha = 0.3f;  /* learning rate */
    float reward = success ? 1.0f : -0.5f;

    /* Factor in execution speed — tasks that took fewer attempts get bonus */
    if (success) {
        reward += 0.2f;  /* success bonus */
    }

    w->weight = w->weight * (1.0f - alpha) + (w->weight + reward * alpha) * alpha;
    /* Clamp weight to reasonable range */
    if (w->weight < 0.1f) w->weight = 0.1f;
    if (w->weight > 3.0f) w->weight = 3.0f;
    w->samples++;
    w->last_updated = time(NULL);

    /* Decay: reduce weight magnitude over time for old entries */
    if (w->samples > 10) {
        w->weight *= 0.95f;  /* slight decay toward 1.0 */
        if (w->weight < 0.5f) w->weight = 0.5f;
    }

    /* Persist to graph */
    save_weight_to_graph(w);

    snprintf(g_last_debug, sizeof(g_last_debug),
             "learn task=%s success=%d seq='%s' weight=%.3f samples=%d",
             task_id, success, seq, w->weight, w->samples);

    free(seq);
}

/* ========================= Init ========================= */

void decision_init(DecisionConfig cfg) {
    g_cfg = cfg;
    srand((unsigned int)time(NULL));
    g_last_debug[0] = 0;
    g_weights_count = 0;
    g_weights_loaded = 0;
}

/* ========================= Debug ========================= */

void decision_debug_last(char *buf, size_t size) {
    strncpy(buf, g_last_debug, size - 1);
    buf[size - 1] = 0;
}
