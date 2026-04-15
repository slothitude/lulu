#pragma once

void llm_init(const char *endpoint, const char *model, const char *apikey, int timeout_ms);

/* Send prompt, return malloc'd content string or NULL */
char *llm_chat(const char *prompt, int max_retries);

/* Multi-pass JSON extraction from LLM response. Returns malloc'd string or NULL. */
char *llm_extract_json(const char *response);

/* Escape a string for JSON embedding. Returns malloc'd string. */
char *llm_escape_json_string(const char *s);
