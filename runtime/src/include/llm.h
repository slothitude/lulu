#pragma once

#include "cJSON.h"

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

/* ========================= Role-Based Pipeline (Phase 6) ========================= */

/* Chat with a specific role's system prompt. Constructs multi-turn messages:
   [0] = system (role prompt + tools + rules), [1..] = user/assistant history.
   Returns malloc'd assistant reply or NULL. */
char *llm_chat_with_role(const char *role_system, const char *tools_list,
                         const char *user_content, int max_retries);

/* Build rich context for a task from graph DB.
   Gathers: goal, recent tasks, semantic memories, script matches.
   Returns malloc'd string (caller frees). */
char *llm_build_context(const char *task_prompt, const char *task_id);

/* Parse structured tool call tags: <tool_call name="..." args="{...}"/>
   Returns cJSON* with {tool, arguments} or NULL. Caller must cJSON_Delete. */
cJSON *llm_parse_tool_call(const char *response);

#define MAX_CRITIC_RETRIES   2
#define MAX_CONTEXT_CHARS    12288

/* ========================= Streaming (Phase 11) ========================= */

/* Streaming token callback. token is a single token string.
   user_data is passed through from llm_chat_stream. */
typedef void (*llm_stream_fn)(const char *token, void *user_data);

/* Streaming chat: calls on_token for each token as it arrives.
   Returns the full accumulated response (caller frees), or NULL on error.
   Falls back to non-streaming if SSE parsing fails. */
char *llm_chat_stream(const char *prompt, int max_retries,
                      llm_stream_fn on_token, void *user_data);

/* ========================= Token Tracking (Phase 11) ========================= */

typedef struct {
    long long total_prompt_tokens;
    long long total_completion_tokens;
    int       request_count;
} LLMTokenUsage;

/* Get cumulative token usage stats. */
LLMTokenUsage llm_token_usage(void);

/* ========================= Multi-Provider (Phase 11) ========================= */

/* Configure a named provider. Call before llm_init or to add alternate providers. */
void llm_add_provider(const char *name, const char *endpoint,
                      const char *api_key, const char *model);

/* Set which provider to use for a given role. NULL provider = use default. */
void llm_set_role_provider(const char *role, const char *provider_name);
