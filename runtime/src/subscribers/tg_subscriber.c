#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tg_subscriber.h"
#include "event_bus.h"
#include "telegram.h"

static long long g_chat_id = 0;

/* ---- Callbacks ---- */

static int on_tool_result(const Event *ev, void *ud) {
    (void)ud;
    /* Send error notifications to Telegram */
    if (!ev->tool_result.success && g_chat_id) {
        char msg[512];
        snprintf(msg, sizeof(msg), "[ERROR] Tool '%s' failed (step %d, iter %d)",
                 ev->tool_result.tool, ev->tool_result.step_id, ev->tool_result.iteration);
        tg_send_message(g_chat_id, msg);
    }
    return 0;
}

static int on_state_update(const Event *ev, void *ud) {
    (void)ud;
    /* Send progress updates for critic evaluations */
    if (g_chat_id && ev->state_update.stage &&
        strcmp(ev->state_update.stage, "critic") == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "[ITER %d] Progress: %.0f%% | %s",
                 ev->state_update.iteration,
                 ev->state_update.progress * 100,
                 ev->state_update.summary ? ev->state_update.summary : "");
        tg_send_message(g_chat_id, msg);
    }
    return 0;
}

static int on_done(const Event *ev, void *ud) {
    (void)ud;
    /* Send completion notification */
    if (g_chat_id) {
        char msg[1024];
        snprintf(msg, sizeof(msg),
                 "[DONE] Agent finished (iter %d)\n%s",
                 ev->done.iteration,
                 ev->done.summary ? ev->done.summary : "");
        tg_send_message(g_chat_id, msg);
    }
    return 0;
}

/* ---- Public API ---- */

void tg_subscriber_init(long long default_chat_id) {
    g_chat_id = default_chat_id;
    event_bus_subscribe(EVENT_TOOL_RESULT,  on_tool_result,  NULL);
    event_bus_subscribe(EVENT_STATE_UPDATE, on_state_update, NULL);
    event_bus_subscribe(EVENT_DONE,         on_done,         NULL);
}
