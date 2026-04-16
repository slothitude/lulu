#pragma once
#include <time.h>
#include "cJSON.h"

#define MAX_MEMORY_FILES   10
#define MAX_MEMORY_ERRORS  5
#define MAX_GOALS          8
#define LLM_LOG_MAX        4096

typedef struct {
    char id[32];
    char text[1024];
    char status[16];      /* "active", "done", "cancelled" */
    time_t created_at;
} Goal;

typedef struct {
    char files_created[MAX_MEMORY_FILES][256];
    int  files_count;
    char known_errors[MAX_MEMORY_ERRORS][256];
    int  errors_count;
    char last_result[512];
    char last_tool[64];
    int  step_count;
    char summary[512];
    /* Goals */
    Goal goals[MAX_GOALS];
    int  goals_count;
    /* Agent stats */
    time_t started_at;
    long long total_messages;
} WorkingMemory;

typedef struct {
    char provider[32];
    char model[64];
    char endpoint[256];
    char apikey[256];
    int  max_iterations;
    int  max_retries;
    double temperature;
    int  http_timeout_ms;
} Config;

/* Load config from JSON file. Returns 1 on success. */
int config_load(Config *cfg, const char *path);

/* Initialize memory, load from file if exists */
void memory_init(WorkingMemory *mem);

/* Load memory from file */
int memory_load(WorkingMemory *mem, const char *path);

/* Save memory to file atomically */
int memory_save(WorkingMemory *mem, const char *path);

/* Update memory after a step */
void memory_update(WorkingMemory *mem, const char *tool, const char *result, int success);

/* Track a created file in memory */
void memory_track_file(WorkingMemory *mem, const char *path);

/* Append a JSONL log entry */
void state_log_step(const char *log_path, int step_id, const char *tool,
                    cJSON *args, cJSON *result, int success,
                    int iteration, const char *stage);

/* Log a planner/critic stage event */
void state_log_stage(const char *log_path, int iteration, const char *stage,
                     const char *summary, int success);

/* Log a raw LLM interaction */
void state_log_llm(const char *log_path, int iteration, const char *stage,
                   const char *prompt_summary, const char *raw_response,
                   const char *parsed_json, int success,
                   const char *prompt_hash, int cache_hit);

/* Read entire file into malloc'd buffer. Returns NULL on failure. */
char *read_file_contents(const char *path);

/* Write buffer to file atomically via .tmp + rename */
int write_file_atomic(const char *path, const char *content);
