#pragma once

#include "cJSON.h"

/* ---- Event types ---- */
typedef enum {
    EVENT_GOAL_SET,       /* new goal injected */
    EVENT_STEP_CREATED,   /* planner produced a step */
    EVENT_TOOL_CALL,      /* actor about to call a tool */
    EVENT_TOOL_RESULT,    /* tool returned a result */
    EVENT_STATE_UPDATE,   /* critic evaluated / stage transition */
    EVENT_DONE,           /* agent finished (goal achieved or stopped) */
    EVENT_LLM_CALL,       /* raw LLM request/response logged */
    EVENT_TYPE_COUNT = 7
} EventType;

/* ---- Tagged union payload ---- */
typedef struct {
    EventType type;
    int       seq;            /* auto-assigned sequence number */

    union {
        struct { const char *goal_text; }                              goal_set;
        struct { int step_id; const char *task; const char *expected; } step_created;
        struct { int step_id; const char *tool; const cJSON *args; }   tool_call;
        struct {
            int         step_id;
            const char *tool;
            const cJSON *args;
            const cJSON *result;
            int         success;
            int         iteration;
        } tool_result;
        struct {
            int         iteration;
            const char *stage;
            const char *summary;
            int         success;
            double      progress;
            double      confidence;
        } state_update;
        struct {
            int         iteration;
            const char *summary;
            int         success;
        } done;
        struct {
            int         iteration;
            const char *stage;
            const char *prompt_summary;
            const char *raw_response;
            const char *parsed_json;
            int         success;
            const char *prompt_hash;
            int         cache_hit;
        } llm_call;
    };
} Event;

/* Callback: return 0 to continue dispatch, non-zero to stop. */
typedef int (*EventCallback)(const Event *ev, void *user_data);

/* ---- API ---- */
#define EVENT_MAX_SUBS 8

void event_bus_init(void);
void event_bus_shutdown(void);

/* Subscribe to a specific event type. Returns slot index or -1 on full. */
int  event_bus_subscribe(EventType type, EventCallback cb, void *user_data);

/* Publish an event synchronously to all subscribers of its type. */
int  event_bus_publish(EventType type, const Event *ev);
