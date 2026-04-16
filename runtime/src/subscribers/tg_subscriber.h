#pragma once

/* Register the Telegram subscriber.
   Needs default chat_id for sending notifications. */
void tg_subscriber_init(long long default_chat_id);
