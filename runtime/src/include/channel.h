#pragma once

/* Unified channel abstraction — CLI and Telegram both produce AgentEvent.
   No mode switching. All channels polled in the same loop. */

#define CH_EVENT_TEXT_MAX 4096
#define CH_EVENT_QUEUE   64
#define MAX_EVENTS_PER_TICK 16

typedef struct {
    char type[16];       /* "cli", "telegram" */
    char action[16];     /* "message", "command", "edit", "reaction" */
    long long chat_id;   /* 0 for CLI */
    char text[CH_EVENT_TEXT_MAX];
    /* Telegram extras */
    long long msg_id;    /* message id for edits */
    char reaction[8];    /* "👍", "👎", etc. */
} AgentEvent;

/* Initialize all configured channels.
   tg_bot_token: if non-empty, Telegram channel activates.
   tg_chat_id:   if non-zero, filter to this chat. */
void channels_init(const char *tg_bot_token, long long tg_chat_id);

/* Poll all channels with timeout (seconds). Returns 1 if event ready. */
int  channels_poll(double timeout);

/* Dequeue next event. Returns 1 if event available. */
int  channels_next(AgentEvent *out);

/* Reply to a channel: chat_id==0 -> stdout, else -> Telegram. */
void channels_reply(long long chat_id, const char *text);

/* Get count of pending events in queue. */
int  channels_pending(void);

/* Check if Telegram channel is active. */
int  channels_telegram_active(void);

/* Shutdown all channels cleanly. */
void channels_shutdown(void);
