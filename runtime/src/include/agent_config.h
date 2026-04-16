#pragma once

#define AC_MAX_RULES      8
#define AC_MAX_FIELD      64
#define AC_MAX_TEXT       4096
#define AC_MAX_PIPELINE   8
#define AC_MAX_TOOL_FLAGS 16

/* One agent's prompt template */
typedef struct {
    char system[AC_MAX_TEXT];
    char output_format[AC_MAX_TEXT];
    char rules[AC_MAX_RULES][512];
    int  rules_count;
} PromptTemplate;

/* Shared config across roles */
typedef struct {
    char tools_list[AC_MAX_TEXT];
} SharedConfig;

/* Pipeline stage definition */
typedef struct {
    char role[32];
    int  enabled;
} PipelineStage;

/* Tool enable/disable flag */
typedef struct {
    char name[64];
    int  enabled;
} ToolFlag;

/* Loop behavior knobs */
typedef struct {
    int   max_iterations;
    int   max_retries;
    int   stuck_threshold;
    int   stagnation_window;
    double stagnation_min_progress;
    double confidence_threshold;
    char  stuck_hint[512];
    int   actor_max_attempts;
    int   critic_log_tail_bytes;
    int   dry_run;
    int   max_steps_per_iteration;
    int   enable_prompt_cache;
    char  cache_path[256];
    int   enable_debug_view;
    int   enable_telegram;
    char  telegram_bot_token[256];
    char  telegram_chat_id[32];
} Behavior;

/* Full agent config loaded from agent.json */
typedef struct {
    SharedConfig    shared;
    PromptTemplate  planner;
    PromptTemplate  actor;
    PromptTemplate  critic;
    PipelineStage   pipeline[AC_MAX_PIPELINE];
    int             pipeline_count;
    ToolFlag        tool_flags[AC_MAX_TOOL_FLAGS];
    int             tool_flags_count;
    Behavior        behavior;
} AgentConfig;

/* Check if a role is enabled in the pipeline */
int agent_pipeline_has_role(const AgentConfig *ac, const char *role);

/* Load agent.json. Returns 1 on success. */
int agent_config_load(AgentConfig *ac, const char *path);
