#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#include "state.h"
#include "llm.h"
#include "tools.h"
#include "sandbox.h"
#include "agent_config.h"
#include "agent_db.h"
#include "event_bus.h"
#include "subscribers/log_subscriber.h"
#include "subscribers/mem_subscriber.h"
#include "subscribers/sdl3_debugger.h"
#include "subscribers/tg_subscriber.h"
#include "channel.h"
#include "tasks.h"
#include "session.h"
#include "decision_engine.h"

/* Portable asprintf for MSVC */
static int port_asprintf(char **ret, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int len = _vscprintf(fmt, ap);
    va_end(ap);
    if (len < 0) { *ret = NULL; return -1; }
    *ret = (char *)malloc(len + 1);
    if (!*ret) return -1;
    va_start(ap, fmt);
    vsnprintf(*ret, len + 1, fmt, ap);
    va_end(ap);
    return len;
}
#define asprintf port_asprintf

/* ========================= Globals ========================= */

static AgentConfig g_agent_cfg;
static Config g_cfg;
static WorkingMemory g_mem;       /* in-memory mirror, backed by graph */
AgentDB g_adb;                    /* graph database — shared across modules */
static char g_base_dir[MAX_PATH];
static char g_workspace_path[MAX_PATH];
static char g_log_path[MAX_PATH];
static char g_memory_path[MAX_PATH];
static char g_tasks_path[MAX_PATH];
static char g_graph_path[MAX_PATH];
static char g_cache_path[MAX_PATH];
static volatile int g_running = 1;
static time_t g_start_time;

/* Limits */
#define MAX_TOOL_STEPS     8
#define MAX_TOOL_RESULT_CHARS 4096
#define CHAT_INPUT_MAX     4096

/* Threading */
static CRITICAL_SECTION g_state_lock;
static CONDITION_VARIABLE g_task_ready;    /* signal: new task available */
static CONDITION_VARIABLE g_task_done;     /* signal: task completed */
static volatile int g_worker_busy = 0;

/* ========================= Ctrl+C Handler ========================= */

static BOOL WINAPI agent_ctrl_handler(DWORD ctrl) {
    if (ctrl == CTRL_C_EVENT || ctrl == CTRL_BREAK_EVENT) {
        g_running = 0;
        return TRUE;
    }
    return FALSE;
}

/* ========================= Banner ========================= */

static void print_banner(void) {
    printf("========================================\n");
    printf(" Lulu v4.0 — Graph-Native Autonomous Agent\n");
    printf("========================================\n\n");
}

/* ========================= System Prompt Builder ========================= */

static void build_system_prompt(char *buf, size_t size) {
    const char *tools_list = g_agent_cfg.shared.tools_list;
    snprintf(buf, size,
        "You are Lulu, a helpful AI assistant with access to tools. "
        "Reply normally for conversation. When you need to use a tool, "
        "respond with ONLY JSON: {\"tool\":\"name\",\"arguments\":{...}}\n\n"
        "Available tools:\n%s",
        tools_list ? tools_list : "(none)");
}

/* ========================= Tool Execution ========================= */

/* Execute a tool by name with args. Returns malloc'd result string. */
static char *execute_tool(const char *tool_name, cJSON *args, const char *workspace) {
    const LoadedTool *tool = tools_find(tool_name);
    if (!tool) {
        char *err;
        asprintf(&err, "Unknown tool: %s", tool_name);
        return err;
    }
    if (!tool->enabled) {
        char *err;
        asprintf(&err, "Tool disabled: %s", tool_name);
        return err;
    }

    char *error = NULL;
    cJSON *result = tool->fn(args, workspace, &error);

    if (error) {
        char *msg;
        asprintf(&msg, "Tool '%s' error: %s", tool_name, error);
        free(error);
        if (result) cJSON_Delete(result);
        return msg;
    }

    if (result) {
        char *rjson = cJSON_PrintUnformatted(result);
        cJSON_Delete(result);

        /* Truncate large tool results to prevent context explosion */
        if (rjson && strlen(rjson) > MAX_TOOL_RESULT_CHARS) {
            rjson[MAX_TOOL_RESULT_CHARS - 16] = 0;
            strcat(rjson, "...[TRUNCATED]");
        }

        char *msg;
        asprintf(&msg, "Tool '%s' succeeded: %s", tool_name, rjson ? rjson : "ok");
        free(rjson);
        return msg;
    }

    return _strdup("Tool executed with no output.");
}

/* Try to parse LLM response as tool call. Returns result if tool call, NULL otherwise.
   Sets *is_tool. */
static char *try_tool_call(const char *response, const char *workspace, int *is_tool) {
    cJSON *json = cJSON_Parse(response);
    if (!json) { *is_tool = 0; return NULL; }

    cJSON *tool_item = cJSON_GetObjectItem(json, "tool");
    if (!cJSON_IsString(tool_item)) {
        cJSON_Delete(json);
        *is_tool = 0;
        return NULL;
    }

    const char *tool_name = tool_item->valuestring;
    cJSON *args = cJSON_GetObjectItem(json, "arguments");
    if (!args) args = cJSON_CreateObject();

    char *result_str = execute_tool(tool_name, args, workspace);
    cJSON_Delete(json);
    *is_tool = 1;
    return result_str;
}

/* ========================= Chat with Tool Loop ========================= */

/* Run LLM → tool → LLM loop until text response or MAX_TOOL_STEPS.
   Adds messages to session history. Returns final text reply (malloc'd) or NULL. */
static char *chat_with_tools(ChatSession *session, const char *workspace) {
    for (int step = 0; step < MAX_TOOL_STEPS; step++) {
        char *reply = llm_chat_multi(session->history, session->hist_count, 3);
        if (!reply) return NULL;

        /* Check if it's a tool call */
        int is_tool = 0;
        char *tool_result = try_tool_call(reply, workspace, &is_tool);

        if (is_tool && tool_result) {
            /* Add assistant + tool result to history, loop */
            ChatMessage *m;
            /* assistant message */
            if (session->hist_count >= SESSION_HISTORY) {
                free(session->history[0].content);
                memmove(&session->history[0], &session->history[1],
                        (session->hist_count - 1) * sizeof(ChatMessage));
                session->hist_count--;
            }
            m = &session->history[session->hist_count];
            strncpy(m->role, "assistant", sizeof(m->role) - 1);
            m->content = _strdup(reply);
            session->hist_count++;

            /* user message (tool result) */
            if (session->hist_count >= SESSION_HISTORY) {
                free(session->history[0].content);
                memmove(&session->history[0], &session->history[1],
                        (session->hist_count - 1) * sizeof(ChatMessage));
                session->hist_count--;
            }
            m = &session->history[session->hist_count];
            strncpy(m->role, "user", sizeof(m->role) - 1);
            m->content = _strdup(tool_result);
            session->hist_count++;

            if (session->chat_id == 0) printf("[TOOL] %s\n", tool_result);

            free(reply);
            free(tool_result);
            continue;
        }

        /* Not a tool call — this is the final text response */
        if (tool_result) free(tool_result);

        /* Add assistant reply to history */
        if (session->hist_count >= SESSION_HISTORY) {
            free(session->history[0].content);
            memmove(&session->history[0], &session->history[1],
                    (session->hist_count - 1) * sizeof(ChatMessage));
            session->hist_count--;
        }
        ChatMessage *m2 = &session->history[session->hist_count];
        strncpy(m2->role, "assistant", sizeof(m2->role) - 1);
        m2->content = _strdup(reply);
        session->hist_count++;

        return reply;
    }

    /* Hit max tool steps */
    return _strdup("(max tool steps reached, stopping)");
}

/* ========================= Task Execution ========================= */

/* Execute a task autonomously with structured phases:
   PHASE 1: Plan — ask LLM to plan approach (populate task->plan)
   PHASE 2: Act — tool execution loop
   PHASE 3: Evaluate — check if task is done */
static void execute_task(Task *t, const char *workspace) {
    if (!t) return;

    printf("[TASK] Executing %s: %s (attempt %d)\n", t->id, t->prompt, t->attempts + 1);

    tasks_lock();
    strncpy(t->status, "running", TASK_STATUS_MAX - 1);
    t->attempts++;
    t->updated_at = time(NULL);
    tasks_save();
    tasks_unlock();

    /* Create temporary session for this task */
    long long task_chat_id = t->chat_id ? t->chat_id : 0;
    session_lock();
    ChatSession *session = session_get_or_create(task_chat_id + 1000000); /* offset to avoid collision */
    char sys_prompt[4096];
    build_system_prompt(sys_prompt, sizeof(sys_prompt));

    /* Clear and rebuild session for this task */
    session_clear(task_chat_id + 1000000);
    session_unlock();

    /* Re-add system prompt */
    if (session->hist_count >= SESSION_HISTORY) {
        free(session->history[0].content);
        memmove(&session->history[0], &session->history[1],
                (session->hist_count - 1) * sizeof(ChatMessage));
        session->hist_count--;
    }
    ChatMessage *sm = &session->history[session->hist_count];
    strncpy(sm->role, "system", sizeof(sm->role) - 1);
    sm->content = _strdup(sys_prompt);
    session->hist_count++;

    /* Build prompt with context from previous attempts */
    char user_msg[TASK_PROMPT_MAX + TASK_STATE_MAX + 512];
    if (t->state[0]) {
        snprintf(user_msg, sizeof(user_msg),
            "Task: %s\n\nPrevious context:\n%s\n\nContinue this task. What's been done so far is noted above.",
            t->prompt, t->state);
    } else {
        snprintf(user_msg, sizeof(user_msg), "Task: %s", t->prompt);
    }

    if (session->hist_count >= SESSION_HISTORY) {
        free(session->history[0].content);
        memmove(&session->history[0], &session->history[1],
                (session->hist_count - 1) * sizeof(ChatMessage));
        session->hist_count--;
    }
    ChatMessage *um = &session->history[session->hist_count];
    strncpy(um->role, "user", sizeof(um->role) - 1);
    um->content = _strdup(user_msg);
    session->hist_count++;

    /* Run tool loop */
    char *result = chat_with_tools(session, workspace);

    if (result) {
        /* Check if LLM thinks it's done */
        int is_done = 1;
        if (strstr(result, "need more") || strstr(result, "not yet") ||
            strstr(result, "still working") || strstr(result, "I'll continue")) {
            is_done = 0;
        }

        if (is_done) {
            tasks_lock();
            tasks_update(t, "done", result);
            tasks_append_state(t, result);
            tasks_unlock();
            printf("[TASK] %s completed: %.100s\n", t->id, result);
            channels_reply(t->chat_id, result);
        } else {
            tasks_lock();
            tasks_append_state(t, result);
            tasks_update(t, "pending", ""); /* Re-queue for next attempt */
            tasks_unlock();
        }
        free(result);
    } else {
        char err[128];
        snprintf(err, sizeof(err), "LLM call failed (attempt %d/%d)", t->attempts, t->max_attempts);
        tasks_lock();
        tasks_append_state(t, err);
        if (t->attempts >= t->max_attempts) {
            tasks_update(t, "failed", err);
            tasks_unlock();
            printf("[TASK] %s failed: %s\n", t->id, err);
            channels_reply(t->chat_id, err);
        } else {
            strncpy(t->status, "pending", TASK_STATUS_MAX - 1);
            tasks_save();
            tasks_unlock();
        }
    }

    /* Clean up task session */
    session_lock();
    session_clear(task_chat_id + 1000000);
    session_unlock();

    /* Log task execution */
    state_log_stage(g_log_path, t->attempts, "task",
                    t->prompt, strcmp(t->status, "done") == 0);

    /* Decision engine learning */
    decision_learn(t->id, strcmp(t->status, "done") == 0);
}

/* ========================= Agent Think ========================= */

/* State-driven reasoning: runs every loop even without events.
   Inspects tasks, memory, can spawn/re-prioritize/clean up. */
static void agent_think(void) {
    /* Nothing to think about if no tasks exist */
    if (tasks_count(NULL) == 0) return;

    /* Clean up old done tasks (keep last 20) */
    int done_count = tasks_count("done");
    if (done_count > 20) {
        /* Let tasks persist but don't act on them */
    }

    /* Check for permanently failed tasks — notify once */
    /* (handled by execute_task already) */
}

/* ========================= Command Handler ========================= */

static void handle_command(const AgentEvent *ev, const char *workspace) {
    const char *text = ev->text;

    if (strcmp(text, "/stop") == 0) {
        channels_reply(ev->chat_id, "Lulu shutting down...");
        g_running = 0;
        return;
    }

    if (strcmp(text, "/clear") == 0) {
        session_clear(ev->chat_id);
        channels_reply(ev->chat_id, "Conversation cleared.");
        return;
    }

    if (strcmp(text, "/status") == 0) {
        char status[512];
        time_t uptime = difftime(time(NULL), g_start_time);
        int h = (int)(uptime / 3600);
        int m = (int)((uptime % 3600) / 60);
        tasks_lock();
        int total = tasks_count(NULL);
        int pending = tasks_count("pending");
        int done = tasks_count("done");
        int failed = tasks_count("failed");
        tasks_unlock();
        int64_t nodes = agent_db_node_count(&g_adb);
        int64_t rels = agent_db_rel_count(&g_adb);
        snprintf(status, sizeof(status),
            "Lulu v4.0  uptime %dh%dm  steps %d  msgs %lld\n"
            "Worker: %s\n"
            "Tasks: %d pending  %d done  %d failed\n"
            "Graph: %lld nodes  %lld edges",
            h, m, g_mem.step_count, g_mem.total_messages,
            g_worker_busy ? "BUSY" : "IDLE",
            pending, done, failed,
            (long long)nodes, (long long)rels);
        channels_reply(ev->chat_id, status);
        return;
    }

    if (strcmp(text, "/files") == 0) {
        const LoadedTool *lf = tools_find("list_files");
        if (lf && lf->enabled) {
            char *err = NULL;
            cJSON *args = cJSON_CreateObject();
            cJSON *res = lf->fn(args, workspace, &err);
            cJSON_Delete(args);
            char *rj = res ? cJSON_PrintUnformatted(res) : _strdup("(no output)");
            if (err) free(err);
            channels_reply(ev->chat_id, rj);
            free(rj);
            if (res) cJSON_Delete(res);
        } else {
            channels_reply(ev->chat_id, "list_files tool not available.");
        }
        return;
    }

    if (strcmp(text, "/tasks") == 0) {
        char buf[2048];
        tasks_lock();
        tasks_list(buf, sizeof(buf));
        tasks_unlock();
        channels_reply(ev->chat_id, buf);
        return;
    }

    if (strcmp(text, "/decide") == 0) {
        char debug[1024];
        decision_debug_last(debug, sizeof(debug));
        char reply[1200];
        snprintf(reply, sizeof(reply), "Last decision: %s", debug);
        channels_reply(ev->chat_id, reply);
        return;
    }

    if (strncmp(text, "/graph ", 7) == 0) {
        const char *query = text + 7;
        /* Security: only allow read-only queries */
        if (strncmp(query, "MATCH", 5) != 0 && strncmp(query, "RETURN", 6) != 0) {
            channels_reply(ev->chat_id, "Only MATCH/RETURN queries allowed.");
            return;
        }
        agent_db_lock();
        ryu_query_result result;
        ryu_state state = ryu_connection_query(&g_adb._conn, query, &result);
        if (state != RyuSuccess || !ryu_query_result_is_success(&result)) {
            char *err = ryu_query_result_get_error_message(&result);
            char msg[512];
            snprintf(msg, sizeof(msg), "Query failed: %s", err ? err : "unknown error");
            if (err) ryu_destroy_string(err);
            channels_reply(ev->chat_id, msg);
            ryu_query_result_destroy(&result);
            agent_db_unlock();
            return;
        }
        char buf[4096]; buf[0] = 0;
        size_t pos = 0;
        uint64_t ncols = ryu_query_result_get_num_columns(&result);
        while (ryu_query_result_has_next(&result) && pos < sizeof(buf) - 256) {
            ryu_flat_tuple tuple;
            ryu_query_result_get_next(&result, &tuple);
            for (uint64_t c = 0; c < ncols; c++) {
                ryu_value val;
                ryu_flat_tuple_get_value(&tuple, c, &val);
                if (ryu_value_is_null(&val)) {
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "NULL\t");
                } else {
                    char *s = NULL;
                    ryu_value_get_string(&val, &s);
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\t", s ? s : "");
                    if (s) ryu_destroy_string(s);
                }
                ryu_value_destroy(&val);
            }
            pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
            ryu_flat_tuple_destroy(&tuple);
        }
        ryu_query_result_destroy(&result);
        agent_db_unlock();
        channels_reply(ev->chat_id, buf[0] ? buf : "(no results)");
        return;
    }

    if (strncmp(text, "/goal ", 6) == 0) {
        const char *goal_text = text + 6;
        if (!goal_text[0]) {
            channels_reply(ev->chat_id, "Usage: /goal <description>");
            return;
        }
        tasks_lock();
        Task *t = tasks_create(goal_text, ev->type, ev->chat_id, 10);
        tasks_unlock();
        if (t) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Task created: %s — I'll work on it.", t->id);
            channels_reply(ev->chat_id, msg);

            /* Add to goals in memory */
            EnterCriticalSection(&g_state_lock);
            if (g_mem.goals_count < MAX_GOALS) {
                Goal *g = &g_mem.goals[g_mem.goals_count++];
                snprintf(g->id, sizeof(g->id), "goal_%d", g_mem.goals_count);
                strncpy(g->text, goal_text, sizeof(g->text) - 1);
                strncpy(g->status, "active", sizeof(g->status) - 1);
                g->created_at = time(NULL);
            }
            LeaveCriticalSection(&g_state_lock);

            /* Wake worker thread */
            WakeConditionVariable(&g_task_ready);
        } else {
            channels_reply(ev->chat_id, "Failed to create task (queue full).");
        }
        return;
    }

    /* Unknown command */
    channels_reply(ev->chat_id,
        "Commands: /stop /clear /status /files /tasks /decide /goal <text> /graph <cypher>");
}

/* ========================= Message Handler ========================= */

static void handle_message(const AgentEvent *ev, const char *workspace) {
    session_lock();
    ChatSession *session = session_get_or_create(ev->chat_id);

    /* Initialize system prompt if new session */
    if (session->hist_count == 0) {
        char sys_prompt[4096];
        build_system_prompt(sys_prompt, sizeof(sys_prompt));

        if (session->hist_count >= SESSION_HISTORY) {
            free(session->history[0].content);
            memmove(&session->history[0], &session->history[1],
                    (session->hist_count - 1) * sizeof(ChatMessage));
            session->hist_count--;
        }
        ChatMessage *sm = &session->history[session->hist_count];
        strncpy(sm->role, "system", sizeof(sm->role) - 1);
        sm->content = _strdup(sys_prompt);
        session->hist_count++;
    }

    /* Add user message */
    if (session->hist_count >= SESSION_HISTORY) {
        free(session->history[0].content);
        memmove(&session->history[0], &session->history[1],
                (session->hist_count - 1) * sizeof(ChatMessage));
        session->hist_count--;
    }
    ChatMessage *m = &session->history[session->hist_count];
    strncpy(m->role, "user", sizeof(m->role) - 1);
    m->content = _strdup(ev->text);
    session->hist_count++;
    session_unlock();

    EnterCriticalSection(&g_state_lock);
    g_mem.total_messages++;
    LeaveCriticalSection(&g_state_lock);

    /* Chat with tools */
    char *reply = chat_with_tools(session, workspace);
    if (reply) {
        channels_reply(ev->chat_id, reply);
        free(reply);
    } else {
        channels_reply(ev->chat_id, "Sorry, I couldn't generate a response.");
        /* Remove the user message to avoid accumulating failures */
        if (session->hist_count > 0) {
            free(session->history[session->hist_count - 1].content);
            session->history[session->hist_count - 1].content = NULL;
            session->hist_count--;
        }
    }
}

/* ========================= One-Shot Mode ========================= */

static int run_one_shot(const char *prompt, const char *stdin_data, const char *workspace) {
    /* Build messages */
    ChatMessage msgs[4];
    int count = 0;
    char sys_prompt[4096];
    build_system_prompt(sys_prompt, sizeof(sys_prompt));

    msgs[0].content = _strdup(sys_prompt);
    strncpy(msgs[0].role, "system", sizeof(msgs[0].role) - 1);
    count = 1;

    /* Combine stdin + arg */
    char user_text[8192];
    if (stdin_data && stdin_data[0]) {
        if (prompt && prompt[0])
            snprintf(user_text, sizeof(user_text), "%s\n\nContext:\n%s", prompt, stdin_data);
        else
            snprintf(user_text, sizeof(user_text), "%s", stdin_data);
    } else {
        snprintf(user_text, sizeof(user_text), "%s", prompt ? prompt : "");
    }

    msgs[1].content = _strdup(user_text);
    strncpy(msgs[1].role, "user", sizeof(msgs[1].role) - 1);
    count = 2;

    /* Tool loop */
    for (int step = 0; step < MAX_TOOL_STEPS; step++) {
        char *reply = llm_chat_multi(msgs, count, 3);
        if (!reply) {
            fprintf(stderr, "[ERROR] LLM call failed\n");
            for (int i = 0; i < count; i++) free(msgs[i].content);
            return 1;
        }

        int is_tool = 0;
        char *tool_result = try_tool_call(reply, workspace, &is_tool);

        if (is_tool && tool_result) {
            fprintf(stderr, "[TOOL] %s\n", tool_result);
            /* Shift history if needed */
            if (count >= 4) {
                free(msgs[0].content);
                memmove(&msgs[0], &msgs[1], (count - 1) * sizeof(ChatMessage));
                count--;
            }
            msgs[count].content = _strdup(reply);
            strncpy(msgs[count].role, "assistant", sizeof(msgs[count].role) - 1);
            count++;
            if (count >= 4) {
                free(msgs[0].content);
                memmove(&msgs[0], &msgs[1], (count - 1) * sizeof(ChatMessage));
                count--;
            }
            msgs[count].content = _strdup(tool_result);
            strncpy(msgs[count].role, "user", sizeof(msgs[count].role) - 1);
            count++;
            free(reply);
            free(tool_result);
            continue;
        }

        /* Final text response */
        if (tool_result) free(tool_result);
        printf("%s\n", reply);
        free(reply);
        for (int i = 0; i < count; i++) free(msgs[i].content);
        return 0;
    }

    for (int i = 0; i < count; i++) free(msgs[i].content);
    return 1;
}

/* ========================= Replay Mode ========================= */

static void replay_print_entry(const char *line) {
    cJSON *entry = cJSON_Parse(line);
    if (!entry) return;

    cJSON *ts_item    = cJSON_GetObjectItem(entry, "ts");
    cJSON *iter_item  = cJSON_GetObjectItem(entry, "iter");
    cJSON *stage_item = cJSON_GetObjectItem(entry, "stage");
    cJSON *type_item  = cJSON_GetObjectItem(entry, "type");
    cJSON *step_item  = cJSON_GetObjectItem(entry, "step");
    cJSON *tool_item  = cJSON_GetObjectItem(entry, "tool");
    cJSON *succ_item  = cJSON_GetObjectItem(entry, "success");
    cJSON *sum_item   = cJSON_GetObjectItem(entry, "summary");

    const char *ts    = cJSON_IsString(ts_item)    ? ts_item->valuestring    : "?";
    int         iter  = cJSON_IsNumber(iter_item)  ? iter_item->valueint     : 0;
    const char *stage = cJSON_IsString(stage_item) ? stage_item->valuestring : "?";
    const char *type  = cJSON_IsString(type_item)  ? type_item->valuestring  : NULL;

    if (type && strcmp(type, "llm") == 0) {
        cJSON *prompt_item = cJSON_GetObjectItem(entry, "prompt_summary");
        cJSON *phash_item  = cJSON_GetObjectItem(entry, "prompt_hash");
        cJSON *chit_item   = cJSON_GetObjectItem(entry, "cache_hit");
        const char *prompt = cJSON_IsString(prompt_item) ? prompt_item->valuestring : "";
        const char *phash  = cJSON_IsString(phash_item)  ? phash_item->valuestring  : "";
        int cache_hit = cJSON_IsBool(chit_item) ? chit_item->valueint : 0;
        int success = cJSON_IsBool(succ_item) ? succ_item->valueint : 0;
        printf("[%s] %s/llm | iter=%d | %s | %s | hash=%s%s\n",
               ts, stage, iter, success ? "OK" : "FAIL",
               prompt, phash,
               cache_hit ? " [CACHED]" : "");
    } else if (sum_item && cJSON_IsString(sum_item)) {
        printf("[%s] %s | iter=%d | %s\n", ts, stage, iter, sum_item->valuestring);
    } else {
        const char *tool = cJSON_IsString(tool_item) ? tool_item->valuestring : "?";
        int step = cJSON_IsNumber(step_item) ? step_item->valueint : 0;
        int success = cJSON_IsBool(succ_item) ? succ_item->valueint : 0;
        printf("[%s] %s | iter=%d step=%d | tool=%s | %s\n",
               ts, stage, iter, step, tool, success ? "OK" : "FAIL");
    }

    cJSON_Delete(entry);
}

static int run_replay(const char *log_path, const char *filter_stage, int filter_last) {
    FILE *f = fopen(log_path, "r");
    if (!f) {
        fprintf(stderr, "[REPLAY] Cannot open %s\n", log_path);
        return 1;
    }

    char **ring = NULL;
    int ring_size = 0;
    int ring_idx = 0;
    int match_count = 0;
    int total_count = 0;

    if (filter_last > 0) {
        ring_size = filter_last;
        ring = (char **)calloc(ring_size, sizeof(char *));
    }

    char line[8192];

    printf("[REPLAY] Log: %s\n", log_path);
    if (filter_stage) printf("[REPLAY] Filter: stage=%s\n", filter_stage);
    if (filter_last > 0) printf("[REPLAY] Filter: last=%d\n", filter_last);
    printf("\n");

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
        if (len == 0) continue;

        total_count++;

        if (filter_stage) {
            cJSON *entry = cJSON_Parse(line);
            if (!entry) continue;
            cJSON *stage_item = cJSON_GetObjectItem(entry, "stage");
            const char *stage = cJSON_IsString(stage_item) ? stage_item->valuestring : NULL;
            int matches = (stage && strcmp(stage, filter_stage) == 0);
            cJSON_Delete(entry);
            if (!matches) continue;
        }

        match_count++;

        if (filter_last > 0) {
            free(ring[ring_idx % ring_size]);
            ring[ring_idx % ring_size] = _strdup(line);
            ring_idx++;
        } else {
            replay_print_entry(line);
        }
    }

    fclose(f);

    if (filter_last > 0 && ring) {
        int count = (ring_idx < ring_size) ? ring_idx : ring_size;
        int start = (ring_idx < ring_size) ? 0 : (ring_idx % ring_size);
        for (int i = 0; i < count; i++) {
            int idx = (start + i) % ring_size;
            if (ring[idx]) {
                replay_print_entry(ring[idx]);
                free(ring[idx]);
            }
        }
        free(ring);
    }

    printf("\n[REPLAY] %d entries displayed (%d total)\n", match_count, total_count);
    return 0;
}

/* ========================= Read Stdin (for pipe detection) ========================= */

static char *read_stdin_all(void) {
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    /* If stdin is a pipe (not console), read all content */
    if (GetConsoleMode(hStdin, &mode)) return NULL; /* it's a console, not piped */

    /* Read from pipe */
    char *buf = (char *)malloc(65536);
    if (!buf) return NULL;
    size_t total = 0;
    size_t capacity = 65536;

    while (1) {
        DWORD bytes_read;
        if (!ReadFile(hStdin, buf + total, (DWORD)(capacity - total - 1), &bytes_read, NULL))
            break;
        if (bytes_read == 0) break;
        total += bytes_read;
        if (total >= capacity - 1) {
            capacity *= 2;
            char *new_buf = (char *)realloc(buf, capacity);
            if (!new_buf) { free(buf); return NULL; }
            buf = new_buf;
        }
    }
    buf[total] = 0;
    if (total == 0) { free(buf); return NULL; }
    return buf;
}

/* ========================= Core Init ========================= */

static int core_init(const char *tools_dir) {
    char config_path[MAX_PATH];
    char agent_cfg_path[MAX_PATH];

    snprintf(config_path, MAX_PATH, "%s\\config.json", g_base_dir);
    snprintf(agent_cfg_path, MAX_PATH, "%s\\agent.json", g_base_dir);

    /* ===== Graph Database ===== */
    agent_db_init_lock();
    if (!agent_db_open(&g_adb, g_graph_path)) {
        fprintf(stderr, "[FATAL] Could not open graph database at %s\n", g_graph_path);
        return 0;
    }
    printf("[INIT] Graph database opened\n");

    /* ===== Migration from v3 ===== */
    if (agent_db_needs_migration(&g_adb)) {
        int migrated = 0;
        migrated += agent_db_migrate_tasks_json(&g_adb, g_tasks_path);
        migrated += agent_db_migrate_memory_json(&g_adb, g_memory_path);
        migrated += agent_db_migrate_cache_json(&g_adb, g_cache_path);
        if (migrated > 0) {
            printf("[INIT] Migrated %d records from v3\n", migrated);
            /* Rename v3 files */
            {
                char bak[MAX_PATH];
                snprintf(bak, MAX_PATH, "%s\\state\\tasks.json.v3.bak", g_base_dir);
                rename(g_tasks_path, bak);
                snprintf(bak, MAX_PATH, "%s\\state\\memory.json.v3.bak", g_base_dir);
                rename(g_memory_path, bak);
                snprintf(bak, MAX_PATH, "%s\\state\\prompt_cache.json.v3.bak", g_base_dir);
                rename(g_cache_path, bak);
            }
        }
    }

    if (!config_load(&g_cfg, config_path)) {
        fprintf(stderr, "[FATAL] Could not load config from %s\n", config_path);
        return 0;
    }
    printf("[INIT] Config loaded: model=%s\n", g_cfg.model);

    if (!agent_config_load(&g_agent_cfg, agent_cfg_path)) {
        fprintf(stderr, "[FATAL] Could not load agent config from %s\n", agent_cfg_path);
        return 0;
    }
    printf("[INIT] Agent config loaded\n");

    if (g_agent_cfg.behavior.max_iterations > 0)
        g_cfg.max_iterations = g_agent_cfg.behavior.max_iterations;

    llm_init(g_cfg.endpoint, g_cfg.model, g_cfg.apikey, g_cfg.http_timeout_ms);

    /* Prompt cache */
    {
        char cache_file_path[MAX_PATH];
        if (g_agent_cfg.behavior.cache_path[0]) {
            snprintf(cache_file_path, MAX_PATH, "%s\\%s", g_base_dir, g_agent_cfg.behavior.cache_path);
        } else {
            snprintf(cache_file_path, MAX_PATH, "%s\\state\\prompt_cache.json", g_base_dir);
        }
        llm_cache_init(g_agent_cfg.behavior.enable_prompt_cache, cache_file_path);
        if (g_agent_cfg.behavior.enable_prompt_cache) {
            llm_cache_load();
            printf("[INIT] Prompt cache enabled\n");
        }
    }

    int tool_count = tools_load_all(tools_dir);
    if (tool_count == 0) {
        fprintf(stderr, "[FATAL] No tools loaded from %s\n", tools_dir);
        return 0;
    }
    printf("[INIT] %d tools loaded\n", tool_count);

    /* Apply tool flags */
    for (int i = 0; i < g_agent_cfg.tool_flags_count; i++) {
        tools_set_enabled(g_agent_cfg.tool_flags[i].name, g_agent_cfg.tool_flags[i].enabled);
    }

    /* Build tools_list */
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
        }
    }

    /* Memory */
    memory_init(&g_mem);
    memory_load(&g_mem, g_memory_path);
    g_mem.started_at = time(NULL);

    /* Decision engine */
    DecisionConfig dcfg = { .epsilon = 0.1f, .max_candidates = 10 };
    decision_init(dcfg);
    printf("[INIT] Decision engine initialized (epsilon=%.2f)\n", dcfg.epsilon);

    /* Event bus */
    event_bus_init();
    log_subscriber_init(g_log_path);
    mem_subscriber_init(&g_mem, g_memory_path);

    return 1;
}

/* ========================= Worker Thread ========================= */

static DWORD WINAPI worker_thread_func(LPVOID param) {
    time_t last_prune = 0;
    int save_tick = 0;

    while (g_running) {
        /* Agent think — state-driven reasoning */
        agent_think();

        /* Get next runnable task via decision engine */
        char picked_id[TASK_ID_MAX] = {0};
        int has_task = decision_pick_task(picked_id);
        Task *t = NULL;
        if (has_task) {
            tasks_lock();
            t = tasks_find(picked_id);
            tasks_unlock();
        }

        if (t) {
            g_worker_busy = 1;
            execute_task(t, g_workspace_path);
            g_worker_busy = 0;
            /* Check for more tasks immediately */
            continue;
        }

        /* Periodic save (~every 50 idle iterations) */
        if (++save_tick >= 50) {
            EnterCriticalSection(&g_state_lock);
            memory_save(&g_mem, g_memory_path);
            LeaveCriticalSection(&g_state_lock);
            tasks_lock();
            tasks_save();
            tasks_unlock();
            save_tick = 0;
        }

        /* Prune old sessions every 60s */
        time_t now = time(NULL);
        if (difftime(now, last_prune) > 60) {
            session_lock();
            session_prune(3600); /* 1 hour */
            session_unlock();
            last_prune = now;
        }

        /* Wait for new task or timeout */
        EnterCriticalSection(&g_state_lock);
        SleepConditionVariableCS(&g_task_ready, &g_state_lock, 2000);
        LeaveCriticalSection(&g_state_lock);
    }

    return 0;
}

/* ========================= Core Agent Loop ========================= */

static void run_agent(void) {
    /* Determine TG config */
    const char *tg_token = "";
    long long tg_chat_id = 0;
    if (g_agent_cfg.behavior.enable_telegram && g_agent_cfg.behavior.telegram_bot_token[0]) {
        tg_token = g_agent_cfg.behavior.telegram_bot_token;
        if (g_agent_cfg.behavior.telegram_chat_id[0])
            tg_chat_id = strtoll(g_agent_cfg.behavior.telegram_chat_id, NULL, 10);
    }

    /* Initialize channels */
    channels_init(tg_token, tg_chat_id);

    if (channels_telegram_active()) {
        printf("[INIT] Telegram connected (chat_id=%lld)\n", tg_chat_id);
        /* TG subscriber for event bus */
        tg_subscriber_init(tg_chat_id);
    }

    /* Initialize thread safety */
    InitializeCriticalSection(&g_state_lock);
    InitializeConditionVariable(&g_task_ready);
    InitializeConditionVariable(&g_task_done);
    tasks_init_lock();
    session_init_lock();

    /* Load tasks */
    tasks_load(g_tasks_path);
    int resumed = tasks_count("pending") + tasks_count("failed");
    if (resumed > 0) printf("[INIT] %d tasks to resume\n", resumed);

    /* SDL3 debugger */
    {
        int debug = g_agent_cfg.behavior.enable_debug_view;
        if (debug) sdl3_debugger_init();
    }

    /* Announce */
    printf("[INIT] Lulu online. Type a message or /help for commands.\n\n");
    channels_reply(0, "Lulu online.");

    SetConsoleCtrlHandler(agent_ctrl_handler, TRUE);
    g_start_time = time(NULL);

    /* Create worker thread */
    HANDLE worker = CreateThread(NULL, 0, worker_thread_func, NULL, 0, NULL);
    if (!worker) {
        fprintf(stderr, "[FATAL] Could not create worker thread\n");
        return;
    }

    /* ===== I/O Loop (main thread) ===== */
    while (g_running) {
        int processed = 0;
        while (channels_poll(0.1) && processed < MAX_EVENTS_PER_TICK) {
            AgentEvent ev;
            if (!channels_next(&ev)) break;

            if (strcmp(ev.action, "command") == 0)
                handle_command(&ev, g_workspace_path);
            else
                handle_message(&ev, g_workspace_path);

            processed++;
        }
    }

    /* ===== Shutdown ===== */
    /* Signal worker to exit */
    WakeConditionVariable(&g_task_ready);
    WaitForSingleObject(worker, 5000);
    CloseHandle(worker);

    printf("\n[SHUTDOWN] Saving state...\n");
    memory_save(&g_mem, g_memory_path);
    channels_shutdown();
    session_free_all();

    sdl3_debugger_shutdown();
    event_bus_shutdown();
    tools_cleanup();

    DeleteCriticalSection(&g_state_lock);
    agent_db_close(&g_adb);
    printf("[SHUTDOWN] Done.\n");
}

/* ========================= Main ========================= */

int main(int argc, char *argv[]) {
    print_banner();

    /* Resolve base directory */
#ifdef _WIN32
    GetModuleFileNameA(NULL, g_base_dir, MAX_PATH);
    char *last_sep = strrchr(g_base_dir, '\\');
    if (last_sep) *last_sep = 0;
    char *build_sep = strstr(g_base_dir, "\\build");
    if (build_sep) *build_sep = 0;
    char *runtime_sep = strstr(g_base_dir, "\\runtime");
    if (runtime_sep) *runtime_sep = 0;
#else
    getcwd(g_base_dir, sizeof(g_base_dir));
#endif

    snprintf(g_workspace_path, MAX_PATH, "%s\\workspace", g_base_dir);
    snprintf(g_log_path, MAX_PATH, "%s\\state\\log.jsonl", g_base_dir);
    snprintf(g_memory_path, MAX_PATH, "%s\\state\\memory.json", g_base_dir);
    snprintf(g_tasks_path, MAX_PATH, "%s\\state\\tasks.json", g_base_dir);
    snprintf(g_graph_path, MAX_PATH, "%s\\state\\graph.kuzu", g_base_dir);
    snprintf(g_cache_path, MAX_PATH, "%s\\state\\prompt_cache.json", g_base_dir);

    /* Parse args */
    int replay_mode = 0;
    const char *filter_stage = NULL;
    int filter_last = 0;
    const char *one_shot_prompt = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--replay") == 0) {
            replay_mode = 1;
        } else if (strcmp(argv[i], "--stage") == 0 && i + 1 < argc) {
            filter_stage = argv[++i];
        } else if (strcmp(argv[i], "--last") == 0 && i + 1 < argc) {
            filter_last = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--build") == 0) {
            /* Handled by run.bat */
        } else if (argv[i][0] != '-') {
            /* Positional arg = one-shot prompt */
            one_shot_prompt = argv[i];
        }
    }

    /* Replay mode — no LLM needed */
    if (replay_mode) {
        return run_replay(g_log_path, filter_stage, filter_last);
    }

    /* Initialize core systems */
    char tools_dir[MAX_PATH];
    snprintf(tools_dir, MAX_PATH, "%s\\tools", g_base_dir);

    if (!core_init(tools_dir)) return 1;

    /* Check for piped stdin */
    char *stdin_data = read_stdin_all();

    /* One-shot mode: positional arg or piped stdin */
    if (one_shot_prompt || stdin_data) {
        int rc = run_one_shot(one_shot_prompt, stdin_data, g_workspace_path);
        free(stdin_data);
        llm_cache_save();
        tools_cleanup();
        event_bus_shutdown();
        return rc;
    }

    /* Full agent mode */
    run_agent();
    return 0;
}
