#pragma once

/* TDLib JSON client wrapper for Telegram bot control.

   When ENABLE_TELEGRAM is not defined, all functions are no-ops. */

void tg_init(const char *bot_token);
void tg_shutdown(void);

/* Poll for incoming events. Returns 1 if event received. */
int  tg_poll(double timeout);

/* Get next incoming message. Returns 1 if message available. */
int  tg_get_next_message(long long *chat_id, char *text, size_t text_size);

/* Send text message to chat */
void tg_send_message(long long chat_id, const char *text);

/* Send file with optional caption */
void tg_send_file(long long chat_id, const char *path, const char *caption);

/* Check if Telegram is initialized */
int  tg_is_ready(void);
