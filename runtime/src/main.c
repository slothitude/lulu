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
    snprintf(log_path, MAX_PATH, "%s\\state\\log.txt", base_dir);
    snprintf(memory_path, MAX_PATH, "%s\\state\\memory.json", base_dir);
    snprintf(tools_dir, MAX_PATH, "%s\\tools", base_dir);

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
                print_step_header(iter, steps[s].id, steps[s].task);

                ActorResult ar = actor_run(&steps[s], &mem, workspace_path);

                state_log_step(log_path, steps[s].id, ar.tool, ar.args, ar.result, ar.success);

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
