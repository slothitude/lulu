#pragma once

#include "llm.h"

/* Per-conversation chat history — dynamic sessions via linked list.
   CLI gets chat_id=0, each Telegram chat gets its own. */

#define SESSION_HISTORY 64

typedef struct ChatSession {
    long long chat_id;
    ChatMessage history[SESSION_HISTORY];
    int hist_count;
    struct ChatSession *next;
} ChatSession;

/* Get existing session or create a new one. */
ChatSession *session_get_or_create(long long chat_id);

/* Clear session history (keeps session alive). */
void session_clear(long long chat_id);

/* Free all sessions. */
void session_free_all(void);

/* Get count of active sessions. */
int  session_count(void);
