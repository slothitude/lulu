#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "state.h"
#include "llm.h"
#include "planner.h"
#include "actor.h"
#include "critic.h"
#include "tools.h"
#include "sandbox.h"
#include "agent_config.h"

/* Global agent config — shared with planner/actor/critic */
AgentConfig g_agent_cfg;

static unsigned long hash_string(const char *s) {
    unsigned long h = 5381;
    while (*s) {
        h = ((h << 5) + h) + (unsigned char)*s++;
    }
    return h;
}

static void print_banner(void) {
    printf("========================================\n");
    printf(" BashAgent Hybrid Runtime v1\n");
    printf("========================================\n\n");
}

/* ========================= Replay Mode ========================= */

static int run_replay(const char *log_path) {
    FILE *f = fopen(log_path, "r");
    if (!f) {
        fprintf(stderr, "[REPLAY] Cannot open %s\n", log_path);
        return 1;
    }

    char line[8192];
    int count = 0;
    printf("[REPLAY] Log: %s\n\n", log_path);

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
        if (len == 0) continue;

        cJSON *entry = cJSON_Parse(line);
        if (!entry) continue;

        cJSON *ts_item    = cJSON_GetObjectItem(entry, "ts");
        cJSON *iter_item  = cJSON_GetObjectItem(entry, "iter");
        cJSON *stage_item = cJSON_GetObjectItem(entry, "stage");
        cJSON *step_item  = cJSON_GetObjectItem(entry, "step");
        cJSON *tool_item  = cJSON_GetObjectItem(entry, "tool");
        cJSON *succ_item  = cJSON_GetObjectItem(entry, "success");
        cJSON *sum_item   = cJSON_GetObjectItem(entry, "summary");

        const char *ts    = cJSON_IsString(ts_item)    ? ts_item->valuestring    : "?";
        int         iter  = cJSON_IsNumber(iter_item)  ? iter_item->valueint     : 0;
        const char *stage = cJSON_IsString(stage_item) ? stage_item->valuestring : "?";

        if (sum_item && cJSON_IsString(sum_item)) {
            /* Stage event (planner/critic) */
            printf("[%s] %s | iter=%d | %s\n", ts, stage, iter, sum_item->valuestring);
        } else {
            /* Actor step event */
            const char *tool = cJSON_IsString(tool_item) ? tool_item->valuestring : "?";
            int step = cJSON_IsNumber(step_item) ? step_item->valueint : 0;
            int success = cJSON_IsBool(succ_item) ? succ_item->valueint : 0;

            printf("[%s] %s | iter=%d step=%d | tool=%s | %s\n",
                   ts, stage, iter, step, tool, success ? "OK" : "FAIL");
        }

        cJSON_Delete(entry);
        count++;
    }

    fclose(f);
    printf("\n[REPLAY] %d entries displayed\n", count);
    return 0;
}

static void print_step_header(int iteration, int step_id, const char *task) {
    printf("[ITER %d | STEP %d] %s\n", iteration, step_id, task);
}

static void print_result(const char *tool, cJSON *result, int success) {
    char *r = result ? cJSON_PrintUnformatted(result) : _strdup("(null)");
    printf("  Tool: %s | Success: %s | Result: %s\n",
           tool, success ? "YES" : "NO", r ? r : "(null)");
    free(r);
}

static void print_summary(WorkingMemory *mem, int iterations) {
    printf("\n========================================\n");
    printf(" SUMMARY\n");
    printf("========================================\n");
    printf("Iterations: %d\n", iterations);
    printf("Steps executed: %d\n", mem->step_count);
    printf("Files created: %d\n", mem->files_count);
    for (int i = 0; i < mem->files_count; i++) {
        printf("  - %s\n", mem->files_created[i]);
    }
    printf("Errors: %d\n", mem->errors_count);
    printf("Summary: %s\n", mem->summary[0] ? mem->summary : "N/A");
    printf("========================================\n");
}

int main(int argc, char *argv[]) {
    /* Check for --replay flag */
    int replay_mode = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--replay") == 0) {
            replay_mode = 1;
        }
    }

    print_banner();

    char base_dir[MAX_PATH];
    char config_path[MAX_PATH];
    char agent_cfg_path[MAX_PATH];
    char goal_path[MAX_PATH];
    char workspace_path[MAX_PATH];
    char log_path[MAX_PATH];
    char memory_path[MAX_PATH];
    char tools_dir[MAX_PATH];

#ifdef _WIN32
    GetModuleFileNameA(NULL, base_dir, MAX_PATH);
    char *last_sep = strrchr(base_dir, '\\');
    if (last_sep) *last_sep = 0;

    char *build_sep = strstr(base_dir, "\\build");
    if (build_sep) *build_sep = 0;

    char *runtime_sep = strstr(base_dir, "\\runtime");
    if (runtime_sep) *runtime_sep = 0;
#else
    getcwd(base_dir, sizeof(base_dir));
#endif

    snprintf(config_path, MAX_PATH, "%s\\config.json", base_dir);
    snprintf(agent_cfg_path, MAX_PATH, "%s\\agent.json", base_dir);
    snprintf(goal_path, MAX_PATH, "%s\\state\\info.md", base_dir);
    snprintf(workspace_path, MAX_PATH, "%s\\workspace", base_dir);
    snprintf(log_path, MAX_PATH, "%s\\state\\log.jsonl", base_dir);
    snprintf(memory_path, MAX_PATH, "%s\\state\\memory.json", base_dir);
    snprintf(tools_dir, MAX_PATH, "%s\\tools", base_dir);

    /* Replay mode: read log and exit, no LLM calls */
    if (replay_mode) {
        return run_replay(log_path);
    }

    /* Load LLM config */
    Config cfg;
    if (!config_load(&cfg, config_path)) {
        fprintf(stderr, "[FATAL] Could not load config from %s\n", config_path);
        return 1;
    }
    printf("[INIT] Config loaded: model=%s\n", cfg.model);

    /* Load agent config (roles + pipeline + tools + behavior) */
    if (!agent_config_load(&g_agent_cfg, agent_cfg_path)) {
        fprintf(stderr, "[FATAL] Could not load agent config from %s\n", agent_cfg_path);
        return 1;
    }
    printf("[INIT] Agent config loaded\n");

    /* Let agent.json override max_iterations if present */
    if (g_agent_cfg.behavior.max_iterations > 0) {
        cfg.max_iterations = g_agent_cfg.behavior.max_iterations;
    }

    /* Initialize LLM */
    llm_init(cfg.endpoint, cfg.model, cfg.apikey, cfg.http_timeout_ms);

    /* Load tool DLLs */
    int tool_count = tools_load_all(tools_dir);
    if (tool_count == 0) {
        fprintf(stderr, "[FATAL] No tools loaded from %s\n", tools_dir);
        return 1;
    }
    printf("[INIT] %d tools loaded\n", tool_count);

    /* Apply tool enable/disable flags from config */
    for (int i = 0; i < g_agent_cfg.tool_flags_count; i++) {
        tools_set_enabled(g_agent_cfg.tool_flags[i].name, g_agent_cfg.tool_flags[i].enabled);
        if (!g_agent_cfg.tool_flags[i].enabled) {
            printf("[INIT] Tool disabled: %s\n", g_agent_cfg.tool_flags[i].name);
        }
    }

    /* Build dynamic tools_list from loaded+enabled DLLs */
    {
        char *names = tools_get_enabled_names();
        if (names) {
            char list_buf[AC_MAX_TEXT];
            list_buf[0] = 0;
            char *save = NULL;
            char *name = strtok_s(names, ";", &save);
            int first = 1;
            while (name) {
                const LoadedTool *lt = tools_find(name);
                if (lt) {
                    if (!first) strcat(list_buf, "\n");
                    char entry[512];
                    snprintf(entry, sizeof(entry), "- %s: %s", lt->name,
                             lt->description[0] ? lt->description : "No description");
                    strcat(list_buf, entry);
                    first = 0;
                }
                name = strtok_s(NULL, ";", &save);
            }
            strncpy(g_agent_cfg.shared.tools_list, list_buf, sizeof(g_agent_cfg.shared.tools_list) - 1);
            free(names);
            printf("[INIT] Dynamic tools_list: %s\n", g_agent_cfg.shared.tools_list);
        }
    }

    /* Read goal */
    char *goal = read_file_contents(goal_path);
    if (!goal) {
        fprintf(stderr, "[FATAL] Could not read goal from %s\n", goal_path);
        tools_cleanup();
        return 1;
    }
    printf("[INIT] Goal loaded: %s\n\n", goal);

    /* Initialize memory */
    WorkingMemory mem;
    memory_init(&mem);
    memory_load(&mem, memory_path);

    /* Clear previous log */
    FILE *logf = fopen(log_path, "w");
    if (logf) fclose(logf);

    /* Behavior knobs from agent.json */
    int stuck_threshold = g_agent_cfg.behavior.stuck_threshold;
    if (stuck_threshold <= 0) stuck_threshold = 3;

    int stagnation_window = g_agent_cfg.behavior.stagnation_window;
    if (stagnation_window <= 0) stagnation_window = 3;

    double stagnation_min = g_agent_cfg.behavior.stagnation_min_progress;
    if (stagnation_min <= 0) stagnation_min = 0.1;

    double confidence_threshold = g_agent_cfg.behavior.confidence_threshold;
    if (confidence_threshold <= 0) confidence_threshold = 0.7;

    /* ===== MAIN LOOP ===== */
    int needs_plan = 1;
    char planner_hint[512] = {0};
    double last_progress = 0.0;
    int stagnation_count = 0;

    unsigned long *last_hashes = (unsigned long *)calloc(stuck_threshold, sizeof(unsigned long));
    int hash_idx = 0;
    int hash_count = 0;

    PlannerStep *steps = NULL;
    int step_count = 0;

    int has_planner = agent_pipeline_has_role(&g_agent_cfg, "planner");
    int has_actor   = agent_pipeline_has_role(&g_agent_cfg, "actor");
    int has_critic  = agent_pipeline_has_role(&g_agent_cfg, "critic");

    for (int iter = 1; iter <= cfg.max_iterations; iter++) {
        printf("------ Iteration %d ------\n", iter);

        /* PLANNER (if enabled and needed) */
        if (has_planner && needs_plan) {
            if (steps) { free(steps); steps = NULL; }

            steps = planner_run(goal, &mem,
                planner_hint[0] ? planner_hint : NULL,
                &step_count);

            if (!steps || step_count == 0) {
                fprintf(stderr, "[ORCH] Planner failed, stopping\n");
                break;
            }

            needs_plan = 0;
            planner_hint[0] = 0;

            /* Log planner stage */
            {
                char plan_summary[256];
                snprintf(plan_summary, sizeof(plan_summary), "Planned %d steps", step_count);
                state_log_stage(log_path, iter, "planner", plan_summary, 1);
            }

            printf("[PLAN] %d steps:\n", step_count);
            for (int i = 0; i < step_count; i++) {
                printf("  %d. %s\n", steps[i].id, steps[i].task);
            }
            printf("\n");
        }

        /* ACTOR — execute each step (if enabled) */
        if (has_actor) {
            int step_failed = 0;
            for (int s = 0; s < step_count; s++) {
                /* Enforce max_steps_per_iteration cap */
                int max_s = g_agent_cfg.behavior.max_steps_per_iteration;
                if (max_s > 0 && s >= max_s) {
                    printf("[ORCH] max_steps_per_iteration (%d) reached, skipping remaining steps\n", max_s);
                    break;
                }

                print_step_header(iter, steps[s].id, steps[s].task);

                ActorResult ar = actor_run(&steps[s], &mem, workspace_path);

                state_log_step(log_path, steps[s].id, ar.tool, ar.args, ar.result, ar.success, iter, "actor");

                print_result(ar.tool, ar.result, ar.success);

                char *result_str = ar.result ? cJSON_PrintUnformatted(ar.result) : NULL;
                memory_update(&mem, ar.tool, result_str, ar.success);
                free(result_str);

                memory_save(&mem, memory_path);

                /* Stuck detection */
                char hash_input[256];
                snprintf(hash_input, sizeof(hash_input), "%s", ar.tool);
                unsigned long h = hash_string(hash_input);

                last_hashes[hash_idx % stuck_threshold] = h;
                hash_idx++;
                hash_count = (hash_count < stuck_threshold) ? hash_count + 1 : stuck_threshold;

                if (hash_count >= stuck_threshold) {
                    int all_same = 1;
                    for (int k = 1; k < stuck_threshold; k++) {
                        if (last_hashes[k] != last_hashes[0]) { all_same = 0; break; }
                    }
                    if (all_same) {
                        fprintf(stderr, "[ORCH] Stuck detected — forcing replan\n");
                        needs_plan = 1;
                        strncpy(planner_hint,
                                g_agent_cfg.behavior.stuck_hint[0] ? g_agent_cfg.behavior.stuck_hint : "previous strategy failed, try differently",
                                sizeof(planner_hint) - 1);
                        step_failed = 1;
                        if (ar.args) cJSON_Delete(ar.args);
                        if (ar.result) cJSON_Delete(ar.result);
                        break;
                    }
                }

                if (ar.args) cJSON_Delete(ar.args);
                if (ar.result) cJSON_Delete(ar.result);
            }

            if (step_failed) continue;
        }

        /* CRITIC (if enabled) */
        if (has_critic) {
            CriticResult critic = critic_run(log_path, &mem, goal);

            /* Log critic stage */
            state_log_stage(log_path, iter, "critic", critic.summary[0] ? critic.summary : critic.status, 1);

            printf("\n[CRITIC] %s (progress=%.0f%%, confidence=%.0f%%)\n",
                   critic.status, critic.progress * 100, critic.confidence * 100);

            if (critic.status[0] == 'd' && strcmp(critic.status, "done") == 0) {
                printf("\n[DONE] Goal achieved!\n");
                if (critic.summary[0]) printf("  %s\n", critic.summary);
                break;
            }

            if (critic.status[0] == 'r' && strcmp(critic.status, "revise") == 0 && critic.confidence > confidence_threshold) {
                printf("[REVISE] Strategy change requested (confidence=%.0f%%)\n", critic.confidence * 100);
                if (critic.fix_hint[0]) {
                    printf("  Hint: %s\n", critic.fix_hint);
                    strncpy(planner_hint, critic.fix_hint, sizeof(planner_hint) - 1);
                }
                needs_plan = 1;
            }

            /* Stagnation detection */
            if (critic.progress <= last_progress + 0.01) {
                stagnation_count++;
            } else {
                stagnation_count = 0;
            }
            last_progress = critic.progress;

            if (stagnation_count >= stagnation_window && critic.progress < stagnation_min) {
                fprintf(stderr, "[ORCH] Stagnation detected (%d iterations with no progress). Stopping.\n",
                        stagnation_count);
                break;
            }

            if (critic.summary[0]) {
                strncpy(mem.summary, critic.summary, sizeof(mem.summary) - 1);
            }
            memory_save(&mem, memory_path);
        }

        printf("\n");
    }

    print_summary(&mem, cfg.max_iterations);

    free(goal);
    free(last_hashes);
    if (steps) free(steps);
    tools_cleanup();

    return 0;
}
