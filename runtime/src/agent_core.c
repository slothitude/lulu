#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "agent_core.h"
#include "llm.h"

char *agent_build_rules(const PromptTemplate *role) {
    if (role->rules_count == 0) {
        char *s = (char *)malloc(1);
        if (s) s[0] = 0;
        return s;
    }

    size_t size = 1024;
    for (int i = 0; i < role->rules_count; i++)
        size += strlen(role->rules[i]) + 4;

    char *buf = (char *)malloc(size);
    if (!buf) return NULL;
    buf[0] = 0;

    for (int i = 0; i < role->rules_count; i++) {
        strcat(buf, "- ");
        strcat(buf, role->rules[i]);
        strcat(buf, "\n");
    }
    return buf;
}

char *agent_build_prompt(const PromptTemplate *role, const char *tools_list,
                         const char *context_body) {
    char *rules = agent_build_rules(role);
    if (!rules) return NULL;

    size_t size = 8192 + strlen(role->system) + strlen(context_body) +
                  (tools_list ? strlen(tools_list) : 0) +
                  strlen(role->output_format) + strlen(rules);

    char *prompt = (char *)malloc(size);
    if (!prompt) { free(rules); return NULL; }

    if (tools_list) {
        snprintf(prompt, size,
            "%s\n\n"
            "%s\n\n"
            "AVAILABLE TOOLS:\n%s\n\n"
            "%s\n\n"
            "Rules:\n%s",
            role->system,
            context_body,
            tools_list,
            role->output_format,
            rules);
    } else {
        snprintf(prompt, size,
            "%s\n\n"
            "%s\n\n"
            "%s\n\n"
            "Rules:\n%s",
            role->system,
            context_body,
            role->output_format,
            rules);
    }

    free(rules);
    return prompt;
}

AgentRaw agent_call_llm(const char *prompt, int max_retries) {
    AgentRaw r = {NULL, NULL, NULL, 0, {0}, 0};

    /* Compute prompt hash for logging */
    unsigned long long h = llm_hash_prompt(prompt);
    llm_hash_to_hex(h, r.prompt_hash, sizeof(r.prompt_hash));

    char *response = llm_chat(prompt, max_retries);
    r.cache_hit = llm_last_cache_hit();
    if (!response) return r;
    r.llm_response = _strdup(response);
    r.raw_json = llm_extract_json(response);
    free(response);
    if (!r.raw_json) return r;
    r.parsed = cJSON_Parse(r.raw_json);
    return r;
}

AgentRaw agent_run_call(const AgentCall *call) {
    char *prompt = agent_build_prompt(call->role, call->tools_list, call->context_body);
    if (!prompt) return (AgentRaw){NULL, NULL, NULL, 0, {0}, 0};
    AgentRaw r = agent_call_llm(prompt, call->max_retries);
    free(prompt);
    return r;
}

void agent_raw_free(AgentRaw *r) {
    if (!r) return;
    if (r->parsed) cJSON_Delete(r->parsed);
    free(r->raw_json);
    free(r->llm_response);
    r->parsed = NULL;
    r->raw_json = NULL;
    r->llm_response = NULL;
}
