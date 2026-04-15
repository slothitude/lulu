#pragma once
#include "agent_config.h"
#include "cJSON.h"

/* Context for one agent invocation */
typedef struct {
    const PromptTemplate *role;
    const char *tools_list;         /* from shared config, or NULL */
    const char *context_body;       /* role-specific body (caller builds) */
    int max_retries;
} AgentCall;

/* Raw LLM result after extraction */
typedef struct {
    cJSON *parsed;      /* caller owns, must cJSON_Delete */
    char *raw_json;     /* caller owns, must free */
    int retries_used;
} AgentRaw;

/* Build rules text from template — replaces 3 identical copies */
char *agent_build_rules(const PromptTemplate *role);

/* Full prompt from role template + context.
   Layout: {system}\n\n{context_body}\n\n[AVAILABLE TOOLS:\n{tools_list}\n\n]{output_format}\n\nRules:\n{rules}
   tools_list is omitted if NULL. */
char *agent_build_prompt(const PromptTemplate *role, const char *tools_list,
                         const char *context_body);

/* Call LLM and return parsed JSON. Handles retries and extraction.
   Returns {NULL, NULL, 0} on failure. Caller owns parsed cJSON*. */
AgentRaw agent_call_llm(const char *prompt, int max_retries);

/* Convenience: build prompt + call LLM in one shot */
AgentRaw agent_run_call(const AgentCall *call);
