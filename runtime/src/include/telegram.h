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

/* Send message with inline keyboard buttons.
   buttons_json: JSON array of rows, each row is array of {text, callback_data}.
   Example: "[[{\"text\":\"Approve\",\"callback_data\":\"approve\"}]]" */
void tg_send_message_inline(long long chat_id, const char *text, const char *buttons_json);

/* Send message and return the message ID (0 on failure) */
long long tg_send_message_ex(long long chat_id, const char *text);

/* Edit existing message text in-place */
void tg_edit_message(long long chat_id, long long msg_id, const char *new_text);

/* Answer a callback query (dismisses button loading spinner) */
void tg_answer_callback_query(long long query_id, const char *text);

/* Set a reaction on a message */
void tg_set_reaction(long long chat_id, long long msg_id, const char *emoji);

/* Dequeue next callback query. Returns 1 if available. */
int  tg_get_next_callback(long long *chat_id, long long *msg_id,
                           long long *query_id, char *data, size_t data_size);
