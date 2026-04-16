#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "channel.h"
#include "telegram.h"

/* ===== Internal event queue ===== */

static AgentEvent g_event_queue[CH_EVENT_QUEUE];
static int g_event_head = 0;
static int g_event_tail = 0;
static int g_event_count = 0;
static CRITICAL_SECTION g_queue_lock;

static void enqueue_event(const AgentEvent *ev) {
    EnterCriticalSection(&g_queue_lock);
    if (g_event_count >= CH_EVENT_QUEUE) {
        /* Drop oldest */
        g_event_head = (g_event_head + 1) % CH_EVENT_QUEUE;
        g_event_count--;
    }
    g_event_queue[g_event_tail] = *ev;
    g_event_tail = (g_event_tail + 1) % CH_EVENT_QUEUE;
    g_event_count++;
    LeaveCriticalSection(&g_queue_lock);
}

static int dequeue_event(AgentEvent *out) {
    EnterCriticalSection(&g_queue_lock);
    if (g_event_count == 0) {
        LeaveCriticalSection(&g_queue_lock);
        return 0;
    }
    *out = g_event_queue[g_event_head];
    g_event_head = (g_event_head + 1) % CH_EVENT_QUEUE;
    g_event_count--;
    LeaveCriticalSection(&g_queue_lock);
    return 1;
}

/* ===== CLI channel ===== */

static HANDLE g_stdin_handle = NULL;
static int g_cli_initialized = 0;

static void cli_init(void) {
    g_stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    g_cli_initialized = 1;
}

/* Non-blocking check: is there data on stdin? */
static int cli_has_data(int timeout_ms) {
    if (!g_cli_initialized) return 0;
    DWORD result = WaitForSingleObject(g_stdin_handle, timeout_ms);
    return (result == WAIT_OBJECT_0);
}

/* Read a line from stdin (blocking). Returns 1 if line read. */
static int cli_read_line(char *buf, size_t size) {
    if (!fgets(buf, (int)size, stdin)) return 0;
    /* Strip trailing newline */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = 0;
    return (len > 0);
}

/* ===== Telegram channel ===== */

static int g_tg_enabled = 0;
static long long g_tg_chat_id = 0;

/* ===== Public API ===== */

void channels_init(const char *tg_bot_token, long long tg_chat_id) {
    InitializeCriticalSection(&g_queue_lock);

    /* CLI is always available */
    cli_init();

    /* Telegram if configured */
    g_tg_chat_id = tg_chat_id;
    if (tg_bot_token && tg_bot_token[0]) {
        tg_init(tg_bot_token);
        if (tg_is_ready()) {
            g_tg_enabled = 1;
        }
    }
}

int channels_poll(double timeout) {
    int got_event = 0;

    /* 1. Poll CLI (quick non-blocking check) */
    if (cli_has_data(0)) {
        char buf[CH_EVENT_TEXT_MAX];
        if (cli_read_line(buf, sizeof(buf))) {
            AgentEvent ev = {0};
            strncpy(ev.type, "cli", sizeof(ev.type) - 1);
            ev.chat_id = 0;
            strncpy(ev.text, buf, sizeof(ev.text) - 1);

            /* Detect commands vs messages */
            if (ev.text[0] == '/') {
                strncpy(ev.action, "command", sizeof(ev.action) - 1);
            } else {
                strncpy(ev.action, "message", sizeof(ev.action) - 1);
            }
            enqueue_event(&ev);
            got_event = 1;
        }
    }

    /* 2. Poll Telegram */
    if (g_tg_enabled) {
        tg_poll(timeout > 0 ? timeout * 0.8 : 0.05);

        long long cid = 0;
        char text[CH_EVENT_TEXT_MAX];
        while (tg_get_next_message(&cid, text, sizeof(text))) {
            /* Filter to configured chat if set */
            if (g_tg_chat_id != 0 && cid != g_tg_chat_id) continue;

            AgentEvent ev = {0};
            strncpy(ev.type, "telegram", sizeof(ev.type) - 1);
            ev.chat_id = cid;
            strncpy(ev.text, text, sizeof(ev.text) - 1);

            if (ev.text[0] == '/') {
                strncpy(ev.action, "command", sizeof(ev.action) - 1);
            } else {
                strncpy(ev.action, "message", sizeof(ev.action) - 1);
            }
            enqueue_event(&ev);
            got_event = 1;
        }
    } else if (!got_event && timeout > 0) {
        /* No TG, no CLI data — sleep the remainder */
        Sleep((DWORD)(timeout * 1000));
    }

    return got_event || g_event_count > 0;
}

int channels_next(AgentEvent *out) {
    return dequeue_event(out);
}

void channels_reply(long long chat_id, const char *text) {
    if (chat_id == 0) {
        printf("%s\n", text);
        fflush(stdout);
    } else if (g_tg_enabled) {
        tg_send_message(chat_id, text);
    }
}

int channels_pending(void) {
    return g_event_count;
}

int channels_telegram_active(void) {
    return g_tg_enabled;
}

void channels_shutdown(void) {
    if (g_tg_enabled) {
        Sleep(500);
        tg_shutdown();
        g_tg_enabled = 0;
    }
}
