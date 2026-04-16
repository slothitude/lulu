#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "session.h"

/* Linked list of sessions */
static ChatSession *g_sessions = NULL;

/* ===== Thread safety ===== */

static CRITICAL_SECTION g_session_lock;

void session_init_lock(void) { InitializeCriticalSection(&g_session_lock); }
void session_lock(void)      { EnterCriticalSection(&g_session_lock); }
void session_unlock(void)    { LeaveCriticalSection(&g_session_lock); }

/* Helper: add message to session's ring buffer */
static void session_add_message(ChatSession *s, const char *role, const char *content) {
    if (s->hist_count >= SESSION_HISTORY) {
        /* Ring buffer: free oldest, shift */
        free(s->history[0].content);
        memmove(&s->history[0], &s->history[1],
                (s->hist_count - 1) * sizeof(ChatMessage));
        s->hist_count--;
    }
    ChatMessage *m = &s->history[s->hist_count];
    strncpy(m->role, role, sizeof(m->role) - 1);
    m->role[sizeof(m->role) - 1] = 0;
    m->content = _strdup(content ? content : "");
    s->hist_count++;
}

ChatSession *session_get_or_create(long long chat_id) {
    /* Search existing */
    ChatSession *s = g_sessions;
    while (s) {
        if (s->chat_id == chat_id) {
            s->last_active = time(NULL);
            return s;
        }
        s = s->next;
    }

    /* Create new */
    s = (ChatSession *)calloc(1, sizeof(ChatSession));
    if (!s) return NULL;
    s->chat_id = chat_id;
    s->last_active = time(NULL);
    s->next = g_sessions;
    g_sessions = s;
    return s;
}

void session_clear(long long chat_id) {
    ChatSession *s = g_sessions;
    while (s) {
        if (s->chat_id == chat_id) {
            for (int i = 0; i < s->hist_count; i++) {
                free(s->history[i].content);
                s->history[i].content = NULL;
            }
            s->hist_count = 0;
            return;
        }
        s = s->next;
    }
}

void session_free_all(void) {
    ChatSession *s = g_sessions;
    while (s) {
        ChatSession *next = s->next;
        for (int i = 0; i < s->hist_count; i++) {
            free(s->history[i].content);
        }
        free(s);
        s = next;
    }
    g_sessions = NULL;
}

int session_count(void) {
    int count = 0;
    ChatSession *s = g_sessions;
    while (s) { count++; s = s->next; }
    return count;
}

void session_prune(time_t max_age) {
    time_t now = time(NULL);
    ChatSession **pp = &g_sessions;
    while (*pp) {
        ChatSession *s = *pp;
        if (difftime(now, s->last_active) > max_age) {
            *pp = s->next;
            for (int i = 0; i < s->hist_count; i++) {
                free(s->history[i].content);
            }
            free(s);
        } else {
            pp = &s->next;
        }
    }
}
