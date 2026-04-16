#pragma once

/* Register the log subscriber for all relevant events.
   Needs the log_path that stays valid for the agent lifetime. */
void log_subscriber_init(const char *log_path);
