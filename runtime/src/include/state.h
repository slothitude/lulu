#pragma once
#include "cJSON.h"

#define MAX_MEMORY_FILES   10
#define MAX_MEMORY_ERRORS  5

typedef struct {
    char files_created[MAX_MEMORY_FILES][256];
    int  files_count;
    char known_errors[MAX_MEMORY_ERRORS][256];
    int  errors_count;
    char last_result[512];
    char last_tool[64];
    int  step_count;
    char summary[512];
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
                    cJSON *args, cJSON *result, int success);

/* Read entire file into malloc'd buffer. Returns NULL on failure. */
char *read_file_contents(const char *path);

/* Write buffer to file atomically via .tmp + rename */
int write_file_atomic(const char *path, const char *content);
