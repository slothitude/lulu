#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "agent_config.h"
#include "state.h"
#include "cJSON.h"

static void load_prompt_field(cJSON *parent, const char *key, char *dest, size_t dest_size) {
    cJSON *item = cJSON_GetObjectItem(parent, key);
    if (cJSON_IsString(item)) {
        strncpy(dest, item->valuestring, dest_size - 1);
        dest[dest_size - 1] = 0;
    }
}

static void load_prompt_template(cJSON *parent, PromptTemplate *pt) {
    memset(pt, 0, sizeof(PromptTemplate));

    load_prompt_field(parent, "system",        pt->system,        sizeof(pt->system));
    load_prompt_field(parent, "output_format",  pt->output_format, sizeof(pt->output_format));

    /* rules array */
    cJSON *rules = cJSON_GetObjectItem(parent, "rules");
    if (cJSON_IsArray(rules)) {
        int n = cJSON_GetArraySize(rules);
        if (n > AC_MAX_RULES) n = AC_MAX_RULES;
        for (int i = 0; i < n; i++) {
            cJSON *r = cJSON_GetArrayItem(rules, i);
            if (cJSON_IsString(r)) {
                strncpy(pt->rules[i], r->valuestring, sizeof(pt->rules[i]) - 1);
                pt->rules_count = i + 1;
            }
        }
    }
}

static void load_behavior(cJSON *parent, Behavior *b) {
    memset(b, 0, sizeof(Behavior));

    cJSON *item;

    item = cJSON_GetObjectItem(parent, "max_iterations");
    b->max_iterations = cJSON_IsNumber(item) ? item->valueint : 10;

    item = cJSON_GetObjectItem(parent, "max_retries");
    b->max_retries = cJSON_IsNumber(item) ? item->valueint : 3;

    item = cJSON_GetObjectItem(parent, "stuck_threshold");
    b->stuck_threshold = cJSON_IsNumber(item) ? item->valueint : 3;

    item = cJSON_GetObjectItem(parent, "stagnation_window");
    b->stagnation_window = cJSON_IsNumber(item) ? item->valueint : 3;

    item = cJSON_GetObjectItem(parent, "stagnation_min_progress");
    b->stagnation_min_progress = cJSON_IsNumber(item) ? item->valuedouble : 0.1;

    item = cJSON_GetObjectItem(parent, "confidence_threshold");
    b->confidence_threshold = cJSON_IsNumber(item) ? item->valuedouble : 0.7;

    load_prompt_field(parent, "stuck_hint", b->stuck_hint, sizeof(b->stuck_hint));

    item = cJSON_GetObjectItem(parent, "actor_max_attempts");
    b->actor_max_attempts = cJSON_IsNumber(item) ? item->valueint : 3;

    item = cJSON_GetObjectItem(parent, "critic_log_tail_bytes");
    b->critic_log_tail_bytes = cJSON_IsNumber(item) ? item->valueint : 2048;

    item = cJSON_GetObjectItem(parent, "dry_run");
    b->dry_run = cJSON_IsBool(item) ? item->valueint : 0;

    item = cJSON_GetObjectItem(parent, "max_steps_per_iteration");
    b->max_steps_per_iteration = cJSON_IsNumber(item) ? item->valueint : 0;

    item = cJSON_GetObjectItem(parent, "enable_prompt_cache");
    b->enable_prompt_cache = cJSON_IsBool(item) ? item->valueint : 0;

    load_prompt_field(parent, "cache_path", b->cache_path, sizeof(b->cache_path));

    item = cJSON_GetObjectItem(parent, "enable_debug_view");
    b->enable_debug_view = cJSON_IsBool(item) ? item->valueint : 0;

    item = cJSON_GetObjectItem(parent, "enable_telegram");
    b->enable_telegram = cJSON_IsBool(item) ? item->valueint : 0;

    load_prompt_field(parent, "telegram_bot_token", b->telegram_bot_token, sizeof(b->telegram_bot_token));
    load_prompt_field(parent, "telegram_chat_id", b->telegram_chat_id, sizeof(b->telegram_chat_id));
}

int agent_config_load(AgentConfig *ac, const char *path) {
    memset(ac, 0, sizeof(AgentConfig));

    char *data = read_file_contents(path);
    if (!data) return 0;

    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!root) return 0;

    /* Load shared config */
    cJSON *shared = cJSON_GetObjectItem(root, "shared");
    if (shared) {
        load_prompt_field(shared, "tools_list", ac->shared.tools_list, sizeof(ac->shared.tools_list));
    }

    /* Load role templates */
    cJSON *roles = cJSON_GetObjectItem(root, "roles");
    if (roles) {
        cJSON *planner = cJSON_GetObjectItem(roles, "planner");
        if (planner) load_prompt_template(planner, &ac->planner);

        cJSON *actor = cJSON_GetObjectItem(roles, "actor");
        if (actor) load_prompt_template(actor, &ac->actor);

        cJSON *critic = cJSON_GetObjectItem(roles, "critic");
        if (critic) load_prompt_template(critic, &ac->critic);
    }

    /* Load pipeline stages */
    cJSON *pipeline = cJSON_GetObjectItem(root, "pipeline");
    if (cJSON_IsArray(pipeline)) {
        int n = cJSON_GetArraySize(pipeline);
        if (n > AC_MAX_PIPELINE) n = AC_MAX_PIPELINE;
        for (int i = 0; i < n; i++) {
            cJSON *stage = cJSON_GetArrayItem(pipeline, i);
            cJSON *role = cJSON_GetObjectItem(stage, "role");
            cJSON *enabled = cJSON_GetObjectItem(stage, "enabled");
            if (cJSON_IsString(role)) {
                strncpy(ac->pipeline[i].role, role->valuestring, sizeof(ac->pipeline[i].role) - 1);
                ac->pipeline[i].enabled = cJSON_IsBool(enabled) ? enabled->valueint : 1;
                ac->pipeline_count = i + 1;
            }
        }
    }

    /* Load tool enable/disable flags */
    cJSON *tools = cJSON_GetObjectItem(root, "tools");
    if (cJSON_IsObject(tools)) {
        cJSON *t = NULL;
        int idx = 0;
        cJSON_ArrayForEach(t, tools) {
            if (idx >= AC_MAX_TOOL_FLAGS) break;
            strncpy(ac->tool_flags[idx].name, t->string, sizeof(ac->tool_flags[idx].name) - 1);
            ac->tool_flags[idx].enabled = cJSON_IsBool(t) ? t->valueint : 1;
            ac->tool_flags_count = ++idx;
        }
    }

    cJSON *behavior = cJSON_GetObjectItem(root, "behavior");
    if (behavior) load_behavior(behavior, &ac->behavior);

    cJSON_Delete(root);
    return 1;
}

int agent_pipeline_has_role(const AgentConfig *ac, const char *role) {
    for (int i = 0; i < ac->pipeline_count; i++) {
        if (ac->pipeline[i].enabled && strcmp(ac->pipeline[i].role, role) == 0)
            return 1;
    }
    return 0;
}

const PromptTemplate *agent_config_get_role(const AgentConfig *ac, const char *role) {
    if (!ac || !role) return NULL;
    if (strcmp(role, "planner") == 0) return &ac->planner;
    if (strcmp(role, "actor") == 0)   return &ac->actor;
    if (strcmp(role, "critic") == 0)  return &ac->critic;
    return NULL;
}
