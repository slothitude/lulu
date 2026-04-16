#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "session.h"
#include "agent_db.h"
#include "cJSON.h"

/* Graph-backed session storage.
   Sessions and messages live as SESSION/MESSAGE nodes in the graph.
   A small in-memory LRU cache holds the hot-path ring buffer. */

extern AgentDB g_adb;

/* ===== Thread safety ===== */

static CRITICAL_SECTION g_session_lock;

void session_init_lock(void) { InitializeCriticalSection(&g_session_lock); }
void session_lock(void)      { EnterCriticalSection(&g_session_lock); }
void session_unlock(void)    { LeaveCriticalSection(&g_session_lock); }

/* ===== In-memory session cache ===== */

/* Cache up to 8 sessions in memory for fast history access */
#define SESSION_CACHE_MAX 8

typedef struct CachedSession {
    long long chat_id;
    char session_id[64];
    ChatMessage history[SESSION_HISTORY];
    int hist_count;
    time_t last_active;
} CachedSession;

static CachedSession g_cache[SESSION_CACHE_MAX];
static int g_cache_count = 0;

static CachedSession *cache_find(long long chat_id) {
    for (int i = 0; i < g_cache_count; i++) {
        if (g_cache[i].chat_id == chat_id) return &g_cache[i];
    }
    return NULL;
}

static void cache_evict(CachedSession *cs) {
    /* Flush history to graph before evicting */
    if (!cs->session_id[0]) return;
    for (int i = 0; i < cs->hist_count; i++) {
        if (cs->history[i].content) {
            agent_db_message_append(&g_adb, cs->session_id,
                                    cs->history[i].role,
                                    cs->history[i].content, i);
        }
    }
    /* Free content */
    for (int i = 0; i < cs->hist_count; i++) {
        free(cs->history[i].content);
        cs->history[i].content = NULL;
    }
    cs->hist_count = 0;
    cs->chat_id = -1;
    cs->session_id[0] = 0;
}

static CachedSession *cache_get_or_create(long long chat_id) {
    CachedSession *cs = cache_find(chat_id);
    if (cs) {
        cs->last_active = time(NULL);
        return cs;
    }

    /* Find empty slot or evict oldest */
    if (g_cache_count < SESSION_CACHE_MAX) {
        cs = &g_cache[g_cache_count++];
    } else {
        /* Evict oldest */
        time_t oldest = time(NULL);
        int idx = 0;
        for (int i = 0; i < g_cache_count; i++) {
            if (g_cache[i].last_active < oldest) {
                oldest = g_cache[i].last_active;
                idx = i;
            }
        }
        cs = &g_cache[idx];
        cache_evict(cs);
    }

    memset(cs, 0, sizeof(CachedSession));
    cs->chat_id = chat_id;
    cs->last_active = time(NULL);

    /* Get or create graph session */
    char cid_str[32];
    snprintf(cid_str, sizeof(cid_str), "%lld", chat_id);
    char *sid = agent_db_session_get_or_create(&g_adb, cid_str, "cli");
    if (sid) {
        strncpy(cs->session_id, sid, sizeof(cs->session_id) - 1);
        free(sid);

        /* Load recent history from graph */
        cJSON *hist = agent_db_session_history(&g_adb, cs->session_id);
        if (hist && cJSON_IsArray(hist)) {
            int n = cJSON_GetArraySize(hist);
            /* Load last SESSION_HISTORY messages */
            int start = n > SESSION_HISTORY ? n - SESSION_HISTORY : 0;
            for (int i = start; i < n; i++) {
                cJSON *msg = cJSON_GetArrayItem(hist, i);
                cJSON *role = cJSON_GetObjectItem(msg, "role");
                cJSON *content = cJSON_GetObjectItem(msg, "content");
                if (cJSON_IsString(role) && cJSON_IsString(content) &&
                    cs->hist_count < SESSION_HISTORY) {
                    ChatMessage *m = &cs->history[cs->hist_count++];
                    strncpy(m->role, role->valuestring, sizeof(m->role) - 1);
                    m->content = _strdup(content->valuestring);
                }
            }
        }
        if (hist) cJSON_Delete(hist);
    }

    return cs;
}

/* ===== Public API ===== */

ChatSession *session_get_or_create(long long chat_id) {
    /* We need to return a ChatSession pointer. Since we cache internally,
       we need a stable pointer. Use a static array of ChatSession objects
       that mirror the CachedSession data. */
    static ChatSession g_sessions[SESSION_CACHE_MAX];
    static int g_session_init = 0;

    if (!g_session_init) {
        memset(g_sessions, 0, sizeof(g_sessions));
        g_session_init = 1;
    }

    CachedSession *cs = cache_get_or_create(chat_id);

    /* Find matching ChatSession slot */
    ChatSession *s = NULL;
    for (int i = 0; i < SESSION_CACHE_MAX; i++) {
        if (g_sessions[i].chat_id == chat_id) { s = &g_sessions[i]; break; }
    }
    if (!s) {
        for (int i = 0; i < SESSION_CACHE_MAX; i++) {
            if (g_sessions[i].chat_id == 0) { s = &g_sessions[i]; break; }
        }
    }
    if (!s) s = &g_sessions[0]; /* fallback */

    /* Sync cached data into ChatSession */
    s->chat_id = chat_id;
    s->hist_count = cs->hist_count;
    s->last_active = cs->last_active;
    memcpy(s->history, cs->history, sizeof(cs->history));

    /* Store back-reference index */
    s->next = NULL;

    return s;
}

void session_clear(long long chat_id) {
    CachedSession *cs = cache_find(chat_id);
    if (cs) {
        if (cs->session_id[0]) {
            agent_db_session_clear(&g_adb, cs->session_id);
        }
        for (int i = 0; i < cs->hist_count; i++) {
            free(cs->history[i].content);
            cs->history[i].content = NULL;
        }
        cs->hist_count = 0;
    }
}

void session_free_all(void) {
    /* Flush all cached sessions to graph */
    for (int i = 0; i < g_cache_count; i++) {
        cache_evict(&g_cache[i]);
    }
    g_cache_count = 0;
}

int session_count(void) {
    return g_cache_count;
}

void session_prune(time_t max_age) {
    (void)max_age;
    agent_db_session_prune(&g_adb, 3600);
}
