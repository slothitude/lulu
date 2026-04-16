#pragma once

void llm_init(const char *endpoint, const char *model, const char *apikey, int timeout_ms);

/* Send prompt, return malloc'd content string or NULL */
char *llm_chat(const char *prompt, int max_retries);

/* Multi-turn chat support */
typedef struct {
    char role[16];    /* "system", "user", "assistant" */
    char *content;    /* malloc'd, owned by caller */
} ChatMessage;

/* Send multi-turn conversation, return malloc'd assistant reply. No caching. */
char *llm_chat_multi(ChatMessage *msgs, int count, int max_retries);

/* Multi-pass JSON extraction from LLM response. Returns malloc'd string or NULL. */
char *llm_extract_json(const char *response);

/* Escape a string for JSON embedding. Returns malloc'd string. */
char *llm_escape_json_string(const char *s);

/* Prompt cache (FNV-1a 64-bit hash based) */
void llm_cache_init(int enabled, const char *cache_path);
int  llm_cache_load(void);
int  llm_cache_save(void);
void llm_cache_stats(int *hits, int *misses, int *entries);

/* Hash utility — exposed for agent_core to compute prompt_hash for logging */
unsigned long long llm_hash_prompt(const char *prompt);
void llm_hash_to_hex(unsigned long long hash, char *out, size_t out_size);

/* Query last cache hit status (call right after llm_chat) */
int llm_last_cache_hit(void);

/* Generate embedding for text. Returns malloc'd float array and sets *out_dim.
   Returns NULL on failure (endpoint unavailable, parse error, etc). */
float *llm_embed(const char *text, int *out_dim);
