#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log_subscriber.h"
#include "event_bus.h"
#include "state.h"

static const char *g_log_path = NULL;

/* ---- callbacks ---- */

static int on_tool_result(const Event *ev, void *ud) {
    (void)ud;
    state_log_step(g_log_path, ev->tool_result.step_id, ev->tool_result.tool,
                   (cJSON *)ev->tool_result.args, (cJSON *)ev->tool_result.result,
                   ev->tool_result.success, ev->tool_result.iteration, "actor", NULL);
    return 0;
}

static int on_state_update(const Event *ev, void *ud) {
    (void)ud;
    state_log_stage(g_log_path, ev->state_update.iteration,
                    ev->state_update.stage, ev->state_update.summary,
                    ev->state_update.success, NULL);
    return 0;
}

static int on_llm_call(const Event *ev, void *ud) {
    (void)ud;
    state_log_llm(g_log_path, ev->llm_call.iteration, ev->llm_call.stage,
                  ev->llm_call.prompt_summary, ev->llm_call.raw_response,
                  ev->llm_call.parsed_json, ev->llm_call.success,
                  ev->llm_call.prompt_hash, ev->llm_call.cache_hit, NULL);
    return 0;
}

static int on_done(const Event *ev, void *ud) {
    (void)ud;
    state_log_stage(g_log_path, ev->done.iteration, "done",
                    ev->done.summary, ev->done.success, NULL);
    return 0;
}

void log_subscriber_init(const char *log_path) {
    g_log_path = log_path;
    event_bus_subscribe(EVENT_TOOL_RESULT,  on_tool_result,  NULL);
    event_bus_subscribe(EVENT_STATE_UPDATE, on_state_update, NULL);
    event_bus_subscribe(EVENT_LLM_CALL,     on_llm_call,     NULL);
    event_bus_subscribe(EVENT_DONE,         on_done,         NULL);
}
