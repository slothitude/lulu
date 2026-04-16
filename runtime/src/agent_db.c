#define _CRT_SECURE_NO_WARNINGS
#include "agent_db.h"
#include "state.h"
#include "ryu.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ========================= Globals ========================= */

static AgentDB *g_adb_ptr = NULL;

void agent_db_init_lock(void) { /* lock initialized in agent_db_open */ }
void agent_db_lock(void)   { if (g_adb_ptr) EnterCriticalSection(&g_adb_ptr->_lock); }
void agent_db_unlock(void) { if (g_adb_ptr) LeaveCriticalSection(&g_adb_ptr->_lock); }

/* ========================= String Helpers ========================= */

static char *cypher_esc(const char *s) {
    if (!s) return _strdup("");
    size_t len = strlen(s);
    char *out = (char *)malloc(len * 2 + 1);
    if (!out) return _strdup("");
    char *p = out;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\0') break;
        switch (c) {
            case '\'': *p++ = '\\'; *p++ = '\''; break;
            case '\\': *p++ = '\\'; *p++ = '\\'; break;
            case '\n': *p++ = '\\'; *p++ = 'n';  break;
            case '\r': *p++ = '\\'; *p++ = 'r';  break;
            case '\t': *p++ = '\\'; *p++ = 't';  break;
            default:
                if ((unsigned char)c < 0x20)
                    p += sprintf(p, "\\u%04x", (unsigned char)c);
                else *p++ = c;
                break;
        }
    }
    *p = 0;
    return out;
}

static int g_id_counter = 0;
static void gen_id(char *buf, size_t size, const char *prefix) {
    snprintf(buf, size, "%s_%lld_%d", prefix,
             (long long)(time(NULL) * 1000), ++g_id_counter);
}

/* ========================= Query Helpers ========================= */

/* Thread safety guard for public agent_db functions.
   Uses recursive CRITICAL_SECTION so nested calls are safe. */
#define DB_GUARD(adb) EnterCriticalSection(&(adb)->_lock)
#define DB_UNGUARD(adb) LeaveCriticalSection(&(adb)->_lock)

static int db_exec(AgentDB *adb, const char *cypher) {
    ryu_query_result result;
    ryu_state state = ryu_connection_query(&adb->_conn, cypher, &result);
    if (state != RyuSuccess || !ryu_query_result_is_success(&result)) {
        char *err = ryu_query_result_get_error_message(&result);
        if (err) {
            snprintf(adb->_last_error, sizeof(adb->_last_error), "%s", err);
            ryu_destroy_string(err);
        }
        ryu_query_result_destroy(&result);
        return 0;
    }
    ryu_query_result_destroy(&result);
    return 1;
}

/* Get single string from row 0 col 0. Caller must ryu_destroy_string. */
static char *db_get_string(AgentDB *adb, const char *cypher) {
    ryu_query_result result;
    ryu_state state = ryu_connection_query(&adb->_conn, cypher, &result);
    if (state != RyuSuccess || !ryu_query_result_is_success(&result)) {
        ryu_query_result_destroy(&result);
        return NULL;
    }
    char *ret = NULL;
    if (ryu_query_result_has_next(&result)) {
        ryu_flat_tuple tuple;
        ryu_query_result_get_next(&result, &tuple);
        ryu_value val;
        ryu_flat_tuple_get_value(&tuple, 0, &val);
        if (!ryu_value_is_null(&val)) {
            ryu_value_get_string(&val, &ret);
        }
        ryu_value_destroy(&val);
        ryu_flat_tuple_destroy(&tuple);
    }
    ryu_query_result_destroy(&result);
    return ret;  /* caller must ryu_destroy_string */
}

static int64_t db_get_int64(AgentDB *adb, const char *cypher, int64_t def) {
    ryu_query_result result;
    ryu_state state = ryu_connection_query(&adb->_conn, cypher, &result);
    if (state != RyuSuccess || !ryu_query_result_is_success(&result)) {
        ryu_query_result_destroy(&result);
        return def;
    }
    int64_t v = def;
    if (ryu_query_result_has_next(&result)) {
        ryu_flat_tuple tuple;
        ryu_query_result_get_next(&result, &tuple);
        ryu_value val;
        ryu_flat_tuple_get_value(&tuple, 0, &val);
        if (!ryu_value_is_null(&val)) {
            ryu_value_get_int64(&val, &v);
        }
        ryu_value_destroy(&val);
        ryu_flat_tuple_destroy(&tuple);
    }
    ryu_query_result_destroy(&result);
    return v;
}

/* ========================= Schema ========================= */

static const char *SCHEMA_DDL[] = {
    "CREATE NODE TABLE TASK ("
    "  id STRING PRIMARY KEY, name STRING, prompt STRING, status STRING,"
    "  result STRING, source STRING, chat_id INT64, priority INT64,"
    "  attempts INT64, max_attempts INT64, plan STRING, state STRING,"
    "  last_error STRING, created_at INT64, updated_at INT64)",

    "CREATE NODE TABLE MEMORY ("
    "  id STRING PRIMARY KEY, category STRING, key STRING,"
    "  content STRING, val STRING, embedding STRING, created_at INT64)",

    "CREATE NODE TABLE GOAL ("
    "  id STRING PRIMARY KEY, text STRING, status STRING, created_at INT64)",

    "CREATE NODE TABLE SESSION ("
    "  id STRING PRIMARY KEY, chat_id INT64, channel STRING,"
    "  last_active INT64)",

    "CREATE NODE TABLE MESSAGE ("
    "  id STRING PRIMARY KEY, session_id STRING, role STRING,"
    "  content STRING, seq INT64, created_at INT64)",

    "CREATE NODE TABLE TOOL_CALL ("
    "  id STRING PRIMARY KEY, tool STRING, args_json STRING,"
    "  result_json STRING, duration_ms INT64, created_at INT64)",

    "CREATE NODE TABLE PROMPT_CACHE ("
    "  hash STRING PRIMARY KEY, response STRING, hit_count INT64,"
    "  task_id STRING, created_at INT64)",

    "CREATE NODE TABLE FILE ("
    "  path STRING PRIMARY KEY, hash STRING, size INT64, created_at INT64)",

    "CREATE NODE TABLE SCRIPT ("
    "  name STRING PRIMARY KEY, description STRING, tool_sequence STRING,"
    "  success_count INT64, created_at INT64)",

    "CREATE NODE TABLE LOG_EVENT ("
    "  id STRING PRIMARY KEY, type STRING, iteration INT64,"
    "  stage STRING, json_data STRING, success BOOLEAN, created_at INT64)",

    "CREATE REL TABLE CALLED (FROM TASK TO TOOL_CALL)",
    "CREATE REL TABLE USED (FROM TASK TO FILE)",
    "CREATE REL TABLE RECORDED_IN (FROM LOG_EVENT TO TASK)",
    NULL
};

static void init_schema(AgentDB *adb) {
    for (int i = 0; SCHEMA_DDL[i]; i++) {
        db_exec(adb, SCHEMA_DDL[i]); /* ignore "already exists" on reopen */
    }
}

/* ========================= Lifecycle ========================= */

int agent_db_open(AgentDB *adb, const char *db_path) {
    memset(adb, 0, sizeof(AgentDB));
    InitializeCriticalSection(&adb->_lock);

    ryu_system_config config = ryu_default_system_config();
    config.buffer_pool_size = 256 * 1024 * 1024;
    config.max_num_threads = 2;
    config.enable_compression = true;

    ryu_state state = ryu_database_init(db_path, config, &adb->_db);
    if (state != RyuSuccess) {
        fprintf(stderr, "[DB] Failed to open database at %s\n", db_path);
        return 0;
    }

    state = ryu_connection_init(&adb->_db, &adb->_conn);
    if (state != RyuSuccess) {
        fprintf(stderr, "[DB] Failed to create connection\n");
        ryu_database_destroy(&adb->_db);
        return 0;
    }

    init_schema(adb);
    adb->_initialized = 1;
    g_adb_ptr = adb;
    printf("[DB] Graph database opened: %s\n", db_path);
    return 1;
}

void agent_db_close(AgentDB *adb) {
    if (!adb->_initialized) return;
    ryu_connection_destroy(&adb->_conn);
    ryu_database_destroy(&adb->_db);
    DeleteCriticalSection(&adb->_lock);
    adb->_initialized = 0;
    printf("[DB] Graph database closed\n");
}

/* ========================= Task column parser ========================= */

#define COL_S(tup, idx, dst, dsz) do { \
    ryu_value _v; \
    ryu_flat_tuple_get_value(tup, (uint64_t)(idx), &_v); \
    if (!ryu_value_is_null(&_v)) { \
        char *_s; ryu_value_get_string(&_v, &_s); \
        strncpy(dst, _s, dsz - 1); ryu_destroy_string(_s); \
    } \
    ryu_value_destroy(&_v); \
} while(0)

#define COL_I(tup, idx, field) do { \
    ryu_value _v; \
    ryu_flat_tuple_get_value(tup, (uint64_t)(idx), &_v); \
    if (!ryu_value_is_null(&_v)) { \
        int64_t _i; ryu_value_get_int64(&_v, &_i); \
        t->field = (int)_i; \
    } \
    ryu_value_destroy(&_v); \
} while(0)

#define COL_L(tup, idx, field) do { \
    ryu_value _v; \
    ryu_flat_tuple_get_value(tup, (uint64_t)(idx), &_v); \
    if (!ryu_value_is_null(&_v)) { \
        int64_t _i; ryu_value_get_int64(&_v, &_i); \
        t->field = _i; \
    } \
    ryu_value_destroy(&_v); \
} while(0)

static DbTask *parse_task_row(ryu_flat_tuple *tup) {
    DbTask *t = (DbTask *)calloc(1, sizeof(DbTask));
    if (!t) return NULL;
    COL_S(tup, 0,  t->id, 32);
    COL_S(tup, 1,  t->name, 128);
    COL_S(tup, 2,  t->prompt, 4096);
    COL_S(tup, 3,  t->status, 16);
    COL_S(tup, 4,  t->result, 2048);
    COL_S(tup, 5,  t->source, 16);
    COL_L(tup, 6,  chat_id);
    COL_I(tup, 7,  priority);
    COL_I(tup, 8,  attempts);
    COL_I(tup, 9,  max_attempts);
    COL_S(tup, 10, t->plan, 4096);
    COL_S(tup, 11, t->state, 8192);
    COL_S(tup, 12, t->last_error, 1024);
    COL_L(tup, 13, created_at);
    COL_L(tup, 14, updated_at);
    return t;
}

#undef COL_S
#undef COL_I
#undef COL_L

/* Helper to get string from a tuple column. Returns malloc'd copy (free with free()). */
static char *dup_col(ryu_flat_tuple *tup, int idx) {
    ryu_value val;
    ryu_flat_tuple_get_value(tup, (uint64_t)idx, &val);
    char *ret = NULL;
    if (!ryu_value_is_null(&val)) {
        char *s;
        ryu_value_get_string(&val, &s);
        ret = _strdup(s);
        ryu_destroy_string(s);
    }
    ryu_value_destroy(&val);
    return ret;
}

/* ========================= Tasks ========================= */

char *agent_db_task_create(AgentDB *adb, const char *name,
                           const char *prompt, int priority,
                           const char *source, long long chat_id) {
    DB_GUARD(adb);
    char id[64];
    gen_id(id, sizeof(id), "task");

    char *e_name = cypher_esc(name), *e_prompt = cypher_esc(prompt);
    char *e_source = cypher_esc(source);

    char q[16384];
    snprintf(q, sizeof(q),
        "CREATE (t:TASK {id:'%s', name:'%s', prompt:'%s', status:'pending', "
        "result:'', source:'%s', chat_id:%lld, priority:%d, "
        "attempts:0, max_attempts:3, plan:'', state:'', last_error:'', "
        "created_at:%lld, updated_at:%lld})",
        id, e_name, e_prompt, e_source,
        (long long)chat_id, priority,
        (long long)time(NULL), (long long)time(NULL));

    free(e_name); free(e_prompt); free(e_source);
    if (!db_exec(adb, q)) { DB_UNGUARD(adb); return NULL; }
    DB_UNGUARD(adb);
    return _strdup(id);
}

DbTask *agent_db_task_next(AgentDB *adb) {
    DB_GUARD(adb);
    const char *q =
        "MATCH (t:TASK) WHERE t.status = 'pending' OR t.status = 'failed' "
        "RETURN t.id, t.name, t.prompt, t.status, t.result, t.source, "
        "t.chat_id, t.priority, t.attempts, t.max_attempts, "
        "t.plan, t.state, t.last_error, t.created_at, t.updated_at "
        "ORDER BY t.priority DESC, t.created_at ASC";

    ryu_query_result result;
    if (ryu_connection_query(&adb->_conn, q, &result) != RyuSuccess ||
        !ryu_query_result_is_success(&result)) {
        ryu_query_result_destroy(&result);
        DB_UNGUARD(adb);
        return NULL;
    }

    DbTask *best = NULL;
    int best_eff = -1;
    time_t now = time(NULL);

    while (ryu_query_result_has_next(&result)) {
        ryu_flat_tuple tup;
        ryu_query_result_get_next(&result, &tup);
        DbTask *t = parse_task_row(&tup);
        ryu_flat_tuple_destroy(&tup);
        if (!t) continue;

        if (strcmp(t->status, "done") == 0 || strcmp(t->status, "running") == 0) {
            agent_db_task_free(t); continue;
        }
        if (strcmp(t->status, "failed") == 0) {
            int cd = 30 * t->attempts; if (cd > 300) cd = 300;
            if (t->max_attempts > 0 && t->attempts >= t->max_attempts)
                { agent_db_task_free(t); continue; }
            if (difftime(now, t->updated_at) < cd)
                { agent_db_task_free(t); continue; }
        }

        int eff = t->priority;
        if (difftime(now, t->created_at) > 60.0 && t->priority < 10) eff++;
        if (!best || eff > best_eff) {
            if (best) agent_db_task_free(best);
            best = t; best_eff = eff;
        } else agent_db_task_free(t);
    }
    ryu_query_result_destroy(&result);
    DB_UNGUARD(adb);
    return best;
}

int agent_db_task_update(AgentDB *adb, const char *id,
                         const char *status, const char *result,
                         const char *plan, const char *state,
                         const char *last_error) {
    DB_GUARD(adb);
    char q[16384];
    int pos = 0;
    pos += snprintf(q + pos, sizeof(q) - pos, "MATCH (t:TASK {id:'%s'}) SET ", id);
    int first = 1;

    #define SET_FIELD(field, val) do { \
        if (val) { \
            char *_e = cypher_esc(val); \
            pos += snprintf(q + pos, sizeof(q) - pos, "%st." #field " = '%s'", \
                            first ? "" : ", ", _e); \
            free(_e); first = 0; \
        } \
    } while(0)

    SET_FIELD(status, status);
    SET_FIELD(result, result);
    SET_FIELD(plan, plan);
    SET_FIELD(state, state);
    SET_FIELD(last_error, last_error);
    #undef SET_FIELD

    pos += snprintf(q + pos, sizeof(q) - pos, "%st.updated_at = %lld",
                    first ? "" : ", ", (long long)time(NULL));
    int _rc = db_exec(adb, q);
    DB_UNGUARD(adb);
    return _rc;
}

int agent_db_task_append_state(AgentDB *adb, const char *id, const char *text) {
    DB_GUARD(adb);
    char q[512];
    snprintf(q, sizeof(q), "MATCH (t:TASK {id:'%s'}) RETURN t.state", id);
    char *cur = db_get_string(adb, q);
    char *cs = (cur) ? cur : _strdup("");

    char ns[8192];
    size_t cl = strlen(cs), tl = strlen(text);
    if (cl + tl + 2 >= 8192) {
        size_t keep = 8192 - tl - 2;
        if (keep < cl) { memmove(cs, cs + (cl - keep), keep); cs[keep] = 0; }
    }
    snprintf(ns, sizeof(ns), "%s%s%s", cs, cs[0] ? "\n" : "", text);
    if (cur) ryu_destroy_string(cur); else free(cs);

    int _rc = agent_db_task_update(adb, id, NULL, NULL, NULL, ns, NULL);
    DB_UNGUARD(adb);
    return _rc;
}

int agent_db_task_count(AgentDB *adb, const char *status) {
    DB_GUARD(adb);
    char q[256];
    if (status)
        snprintf(q, sizeof(q), "MATCH (t:TASK {status:'%s'}) RETURN count(t)", status);
    else
        snprintf(q, sizeof(q), "MATCH (t:TASK) RETURN count(t)");
    int _rc = (int)db_get_int64(adb, q, 0);
    DB_UNGUARD(adb);
    return _rc;
}

void agent_db_task_list(AgentDB *adb, char *buf, size_t size) {
    DB_GUARD(adb);
    buf[0] = 0;
    const char *q =
        "MATCH (t:TASK) RETURN t.id, t.status, t.priority, t.attempts, "
        "t.max_attempts, t.prompt ORDER BY t.created_at DESC LIMIT 50";

    ryu_query_result result;
    if (ryu_connection_query(&adb->_conn, q, &result) != RyuSuccess ||
        !ryu_query_result_is_success(&result)) {
        ryu_query_result_destroy(&result);
        strncpy(buf, "No tasks (db error).\n", size - 1);
        DB_UNGUARD(adb);
        return;
    }

    size_t pos = 0;
    while (ryu_query_result_has_next(&result) && pos < size - 256) {
        ryu_flat_tuple tup;
        ryu_query_result_get_next(&result, &tup);
        char *id = dup_col(&tup, 0), *st = dup_col(&tup, 1);
        char *pr = dup_col(&tup, 2), *at = dup_col(&tup, 3);
        char *ma = dup_col(&tup, 4), *pm = dup_col(&tup, 5);

        char line[512];
        snprintf(line, sizeof(line), "%s [%s] pri=%s att=%s/%s | %.80s\n",
                 id ? id : "?", st ? st : "?", pr ? pr : "0",
                 at ? at : "0", ma ? ma : "3", pm ? pm : "(no prompt)");
        size_t ll = strlen(line);
        if (pos + ll < size - 1) { memcpy(buf + pos, line, ll); pos += ll; }
        free(id); free(st); free(pr); free(at); free(ma); free(pm);
        ryu_flat_tuple_destroy(&tup);
    }
    ryu_query_result_destroy(&result);
    if (pos == 0) strncpy(buf, "No tasks.\n", size - 1);
    DB_UNGUARD(adb);
}

DbTask *agent_db_task_find(AgentDB *adb, const char *id) {
    DB_GUARD(adb);
    char *e = cypher_esc(id);
    char q[512];
    snprintf(q, sizeof(q),
        "MATCH (t:TASK {id:'%s'}) RETURN t.id, t.name, t.prompt, t.status, "
        "t.result, t.source, t.chat_id, t.priority, t.attempts, t.max_attempts, "
        "t.plan, t.state, t.last_error, t.created_at, t.updated_at", e);
    free(e);

    ryu_query_result result;
    if (ryu_connection_query(&adb->_conn, q, &result) != RyuSuccess ||
        !ryu_query_result_is_success(&result)) {
        ryu_query_result_destroy(&result);
        DB_UNGUARD(adb);
        return NULL;
    }
    DbTask *t = NULL;
    if (ryu_query_result_has_next(&result)) {
        ryu_flat_tuple tup;
        ryu_query_result_get_next(&result, &tup);
        t = parse_task_row(&tup);
        ryu_flat_tuple_destroy(&tup);
    }
    ryu_query_result_destroy(&result);
    DB_UNGUARD(adb);
    return t;
}

int agent_db_task_get_runnable(AgentDB *adb, DbTask **out, int max) {
    DB_GUARD(adb);
    const char *q =
        "MATCH (t:TASK) WHERE t.status = 'pending' OR t.status = 'failed' "
        "RETURN t.id, t.name, t.prompt, t.status, t.result, t.source, "
        "t.chat_id, t.priority, t.attempts, t.max_attempts, "
        "t.plan, t.state, t.last_error, t.created_at, t.updated_at "
        "ORDER BY t.priority DESC, t.created_at ASC LIMIT 64";

    ryu_query_result result;
    if (ryu_connection_query(&adb->_conn, q, &result) != RyuSuccess ||
        !ryu_query_result_is_success(&result)) {
        ryu_query_result_destroy(&result);
        DB_UNGUARD(adb);
        return 0;
    }

    int count = 0;
    time_t now = time(NULL);
    while (ryu_query_result_has_next(&result) && count < max) {
        ryu_flat_tuple tup;
        ryu_query_result_get_next(&result, &tup);
        DbTask *t = parse_task_row(&tup);
        ryu_flat_tuple_destroy(&tup);
        if (!t) continue;
        if (strcmp(t->status, "running") == 0 || strcmp(t->status, "done") == 0)
            { agent_db_task_free(t); continue; }
        if (strcmp(t->status, "failed") == 0) {
            int cd = 30 * t->attempts; if (cd > 300) cd = 300;
            if (t->max_attempts > 0 && t->attempts >= t->max_attempts)
                { agent_db_task_free(t); continue; }
            if (difftime(now, t->updated_at) < cd)
                { agent_db_task_free(t); continue; }
        }
        out[count++] = t;
    }
    ryu_query_result_destroy(&result);
    DB_UNGUARD(adb);
    return count;
}

void agent_db_task_free(DbTask *t) { free(t); }

/* ========================= Memory ========================= */

char *agent_db_memory_add(AgentDB *adb, const char *category,
                          const char *key, const char *content) {
    DB_GUARD(adb);
    char *ec = cypher_esc(category), *ek = cypher_esc(key), *en = cypher_esc(content);

    /* Check for duplicate (same category + key) */
    char q[16384];
    snprintf(q, sizeof(q),
        "MATCH (m:MEMORY {category:'%s', key:'%s'}) RETURN m.id LIMIT 1", ec, ek);
    char *existing = db_get_string(adb, q);
    if (existing) {
        /* Update existing */
        char *ret = _strdup(existing);
        ryu_destroy_string(existing);
        snprintf(q, sizeof(q),
            "MATCH (m:MEMORY {category:'%s', key:'%s'}) "
            "SET m.content = '%s', m.created_at = %lld",
            ec, ek, en, (long long)time(NULL));
        free(ec); free(ek); free(en);
        db_exec(adb, q);
        DB_UNGUARD(adb);
        return ret;
    }

    char id[64]; gen_id(id, sizeof(id), "mem");
    snprintf(q, sizeof(q),
        "CREATE (m:MEMORY {id:'%s', category:'%s', key:'%s', content:'%s', "
        "val:'', embedding:'', created_at:%lld})", id, ec, ek, en, (long long)time(NULL));
    free(ec); free(ek); free(en);
    if (!db_exec(adb, q)) { DB_UNGUARD(adb); return NULL; }
    DB_UNGUARD(adb);
    return _strdup(id);
}

int agent_db_memory_store_embedding(AgentDB *adb, const char *memory_id,
                                     const float *vec, int dim) {
    DB_GUARD(adb);
    if (!vec || dim <= 0) { DB_UNGUARD(adb); return 0; }
    /* Serialize as JSON array string: "[0.1,0.2,...]" */
    size_t bufsz = dim * 16 + 4;
    char *buf = (char *)malloc(bufsz);
    if (!buf) { DB_UNGUARD(adb); return 0; }
    int pos = 0;
    buf[pos++] = '[';
    for (int i = 0; i < dim; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, bufsz - pos, "%.6f", vec[i]);
    }
    buf[pos++] = ']';
    buf[pos] = 0;

    char *e_id = cypher_esc(memory_id), *e_emb = cypher_esc(buf);
    char q[65536];
    snprintf(q, sizeof(q),
        "MATCH (m:MEMORY {id:'%s'}) SET m.embedding = '%s'", e_id, e_emb);
    free(e_id); free(e_emb); free(buf);
    int _rc = db_exec(adb, q);
    DB_UNGUARD(adb);
    return _rc;
}

int agent_db_memory_get_embedding(AgentDB *adb, const char *memory_id,
                                   float **out_vec, int *out_dim) {
    DB_GUARD(adb);
    *out_vec = NULL; *out_dim = 0;
    char *e = cypher_esc(memory_id);
    char q[512];
    snprintf(q, sizeof(q),
        "MATCH (m:MEMORY {id:'%s'}) RETURN m.embedding", e);
    free(e);
    char *s = db_get_string(adb, q);
    if (!s) { DB_UNGUARD(adb); return 0; }
    char *emb_str = _strdup(s);
    ryu_destroy_string(s);
    if (!emb_str || emb_str[0] == 0) { free(emb_str); DB_UNGUARD(adb); return 0; }

    /* Parse JSON array */
    cJSON *arr = cJSON_Parse(emb_str);
    free(emb_str);
    if (!cJSON_IsArray(arr)) { if (arr) cJSON_Delete(arr); DB_UNGUARD(adb); return 0; }
    int dim = cJSON_GetArraySize(arr);
    if (dim <= 0) { cJSON_Delete(arr); DB_UNGUARD(adb); return 0; }
    float *vec = (float *)malloc(dim * sizeof(float));
    if (!vec) { cJSON_Delete(arr); DB_UNGUARD(adb); return 0; }
    for (int i = 0; i < dim; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        vec[i] = (float)(cJSON_IsNumber(item) ? item->valuedouble : 0.0f);
    }
    cJSON_Delete(arr);
    *out_vec = vec;
    *out_dim = dim;
    DB_UNGUARD(adb);
    return 1;
}

cJSON *agent_db_memory_search(AgentDB *adb, const float *vec, int dim, int k) {
    DB_GUARD(adb);
    cJSON *arr = cJSON_CreateArray();
    if (k <= 0) k = 5;

    if (vec && dim > 0) {
        /* Vector path: fetch all MEMORY with non-empty embedding, compute cosine sim */
        const char *q = "MATCH (m:MEMORY) WHERE m.embedding <> '' AND m.embedding IS NOT NULL "
                        "RETURN m.id, m.category, m.key, m.content, m.embedding";
        ryu_query_result result;
        if (ryu_connection_query(&adb->_conn, q, &result) != RyuSuccess ||
            !ryu_query_result_is_success(&result)) {
            ryu_query_result_destroy(&result);
            DB_UNGUARD(adb);
            return arr;
        }

        /* Collect candidates */
        typedef struct { char id[64]; char cat[64]; char key[256]; char content[1024]; double score; } CandEntry;
        CandEntry *cands = NULL;
        int ncands = 0, cap = 64;
        cands = (CandEntry *)malloc(cap * sizeof(CandEntry));

        while (ryu_query_result_has_next(&result)) {
            ryu_flat_tuple tup;
            ryu_query_result_get_next(&result, &tup);
            char *mid = dup_col(&tup, 0);
            char *mcat = dup_col(&tup, 1);
            char *mkey = dup_col(&tup, 2);
            char *mcont = dup_col(&tup, 3);
            char *memb = dup_col(&tup, 4);

            if (mid && memb && memb[0]) {
                cJSON *earr = cJSON_Parse(memb);
                if (cJSON_IsArray(earr)) {
                    int edim = cJSON_GetArraySize(earr);
                    if (edim == dim) {
                        double dot = 0, na = 0, nb = 0;
                        for (int i = 0; i < dim; i++) {
                            cJSON *item = cJSON_GetArrayItem(earr, i);
                            float b = (float)(cJSON_IsNumber(item) ? item->valuedouble : 0.0);
                            dot += (double)vec[i] * b;
                            na += (double)vec[i] * vec[i];
                            nb += (double)b * b;
                        }
                        double denom = sqrt(na) * sqrt(nb);
                        double score = (denom > 0) ? dot / denom : 0;

                        if (ncands >= cap) {
                            cap *= 2;
                            cands = (CandEntry *)realloc(cands, cap * sizeof(CandEntry));
                        }
                        CandEntry *c = &cands[ncands++];
                        strncpy(c->id, mid, 63); c->id[63] = 0;
                        strncpy(c->cat, mcat ? mcat : "", 63);
                        strncpy(c->key, mkey ? mkey : "", 255);
                        if (mcont) { strncpy(c->content, mcont, 1023); c->content[1023] = 0; }
                        else c->content[0] = 0;
                        c->score = score;
                    }
                    cJSON_Delete(earr);
                }
            }
            free(mid); free(mcat); free(mkey); free(mcont); free(memb);
            ryu_flat_tuple_destroy(&tup);
        }
        ryu_query_result_destroy(&result);

        /* Sort by score descending */
        for (int i = 0; i < ncands - 1; i++) {
            int best = i;
            for (int j = i + 1; j < ncands; j++) {
                if (cands[j].score > cands[best].score) best = j;
            }
            if (best != i) { CandEntry tmp = cands[i]; cands[i] = cands[best]; cands[best] = tmp; }
        }

        int nk = (ncands < k) ? ncands : k;
        for (int i = 0; i < nk; i++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "id", cands[i].id);
            cJSON_AddStringToObject(item, "category", cands[i].cat);
            cJSON_AddStringToObject(item, "key", cands[i].key);
            cJSON_AddStringToObject(item, "content", cands[i].content);
            cJSON_AddNumberToObject(item, "score", cands[i].score);
            cJSON_AddItemToArray(arr, item);
        }
        free(cands);
    } else {
        /* Text fallback */
        const char *q = "MATCH (m:MEMORY) WHERE m.category <> 'scalar' "
                        "RETURN m.id, m.category, m.key, m.content LIMIT 20";
        ryu_query_result result;
        if (ryu_connection_query(&adb->_conn, q, &result) != RyuSuccess ||
            !ryu_query_result_is_success(&result)) {
            ryu_query_result_destroy(&result);
            DB_UNGUARD(adb);
            return arr;
        }
        int count = 0;
        while (ryu_query_result_has_next(&result) && count < k) {
            ryu_flat_tuple tup;
            ryu_query_result_get_next(&result, &tup);
            char *mid = dup_col(&tup, 0), *mcat = dup_col(&tup, 1);
            char *mkey = dup_col(&tup, 2), *mcont = dup_col(&tup, 3);
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "id", mid ? mid : "");
            cJSON_AddStringToObject(item, "category", mcat ? mcat : "");
            cJSON_AddStringToObject(item, "key", mkey ? mkey : "");
            cJSON_AddStringToObject(item, "content", mcont ? mcont : "");
            cJSON_AddNumberToObject(item, "score", 0.0);
            cJSON_AddItemToArray(arr, item);
            free(mid); free(mcat); free(mkey); free(mcont);
            ryu_flat_tuple_destroy(&tup);
            count++;
        }
        ryu_query_result_destroy(&result);
    }
    DB_UNGUARD(adb);
    return arr;
}

void agent_db_memory_set_scalar(AgentDB *adb, const char *key, const char *val) {
    DB_GUARD(adb);
    char *ek = cypher_esc(key), *ev = cypher_esc(val);
    char q[16384];
    snprintf(q, sizeof(q),
        "MATCH (m:MEMORY {category:'scalar', key:'%s'}) DELETE m", ek);
    db_exec(adb, q);

    char id[64]; gen_id(id, sizeof(id), "sc");
    snprintf(q, sizeof(q),
        "CREATE (m:MEMORY {id:'%s', category:'scalar', key:'%s', "
        "content:'', val:'%s', embedding:'', created_at:%lld})",
        id, ek, ev, (long long)time(NULL));
    free(ek); free(ev);
    db_exec(adb, q);
    DB_UNGUARD(adb);
}

char *agent_db_memory_get_scalar(AgentDB *adb, const char *key) {
    DB_GUARD(adb);
    char *ek = cypher_esc(key);
    char q[2048];
    snprintf(q, sizeof(q),
        "MATCH (m:MEMORY {category:'scalar', key:'%s'}) RETURN m.val", ek);
    free(ek);
    /* db_get_string returns ryu-allocated string, convert to malloc */
    char *s = db_get_string(adb, q);
    if (!s) { DB_UNGUARD(adb); return NULL; }
    char *ret = _strdup(s);
    ryu_destroy_string(s);
    DB_UNGUARD(adb);
    return ret;
}

void agent_db_memory_increment(AgentDB *adb, const char *key) {
    DB_GUARD(adb);
    char *cur = agent_db_memory_get_scalar(adb, key);
    long long val = 0;
    if (cur) { val = strtoll(cur, NULL, 10); free(cur); }
    char buf[32]; snprintf(buf, sizeof(buf), "%lld", val + 1);
    agent_db_memory_set_scalar(adb, key, buf);
    DB_UNGUARD(adb);
}

int agent_db_memory_step_count(AgentDB *adb) {
    char *s = agent_db_memory_get_scalar(adb, "step_count");
    int v = 0;
    if (s) { v = atoi(s); free(s); }
    return v;
}

/* ========================= Goals ========================= */

int agent_db_goal_create(AgentDB *adb, const char *text) {
    DB_GUARD(adb);
    char id[64]; gen_id(id, sizeof(id), "goal");
    char *e = cypher_esc(text);
    char q[4096];
    snprintf(q, sizeof(q),
        "CREATE (g:GOAL {id:'%s', text:'%s', status:'active', created_at:%lld})",
        id, e, (long long)time(NULL));
    free(e);
    int _rc = db_exec(adb, q);
    DB_UNGUARD(adb);
    return _rc;
}

/* ========================= Sessions & Messages ========================= */

char *agent_db_session_get_or_create(AgentDB *adb, const char *chat_id,
                                     const char *channel) {
    DB_GUARD(adb);
    char *ec = cypher_esc(chat_id);
    char q[512];
    snprintf(q, sizeof(q),
        "MATCH (s:SESSION {chat_id:%s}) RETURN s.id ORDER BY s.last_active DESC LIMIT 1",
        ec);
    free(ec);

    char *sid = db_get_string(adb, q);
    if (sid) {
        char *ret = _strdup(sid);
        ryu_destroy_string(sid);
        char uq[512];
        snprintf(uq, sizeof(uq),
            "MATCH (s:SESSION {id:'%s'}) SET s.last_active = %lld",
            ret, (long long)time(NULL));
        db_exec(adb, uq);
        DB_UNGUARD(adb);
        return ret;
    }

    char id[64]; gen_id(id, sizeof(id), "sess");
    char *ech = cypher_esc(channel);
    ec = cypher_esc(chat_id);
    snprintf(q, sizeof(q),
        "CREATE (s:SESSION {id:'%s', chat_id:%s, channel:'%s', last_active:%lld})",
        id, ec, ech, (long long)time(NULL));
    free(ec); free(ech);
    if (!db_exec(adb, q)) { DB_UNGUARD(adb); return NULL; }
    DB_UNGUARD(adb);
    return _strdup(id);
}

char *agent_db_message_append(AgentDB *adb, const char *session_id,
                               const char *role, const char *content, int seq) {
    DB_GUARD(adb);
    char id[64]; gen_id(id, sizeof(id), "msg");
    char *es = cypher_esc(session_id), *er = cypher_esc(role);
    char *ec = cypher_esc(content);

    char q[32768];
    snprintf(q, sizeof(q),
        "CREATE (m:MESSAGE {id:'%s', session_id:'%s', role:'%s', "
        "content:'%s', seq:%d, created_at:%lld})",
        id, es, er, ec, seq, (long long)time(NULL));
    free(es); free(er); free(ec);
    if (!db_exec(adb, q)) { DB_UNGUARD(adb); return NULL; }
    DB_UNGUARD(adb);
    return _strdup(id);
}

cJSON *agent_db_session_history(AgentDB *adb, const char *session_id) {
    DB_GUARD(adb);
    char *es = cypher_esc(session_id);
    char q[512];
    snprintf(q, sizeof(q),
        "MATCH (m:MESSAGE {session_id:'%s'}) "
        "RETURN m.role, m.content ORDER BY m.seq ASC LIMIT 128", es);
    free(es);

    cJSON *arr = cJSON_CreateArray();
    ryu_query_result result;
    if (ryu_connection_query(&adb->_conn, q, &result) != RyuSuccess ||
        !ryu_query_result_is_success(&result)) {
        ryu_query_result_destroy(&result);
        DB_UNGUARD(adb);
        return arr;
    }

    while (ryu_query_result_has_next(&result)) {
        ryu_flat_tuple tup;
        ryu_query_result_get_next(&result, &tup);
        char *role = dup_col(&tup, 0);
        char *content = dup_col(&tup, 1);
        if (role && content) {
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "role", role);
            cJSON_AddStringToObject(msg, "content", content);
            cJSON_AddItemToArray(arr, msg);
        }
        free(role); free(content);
        ryu_flat_tuple_destroy(&tup);
    }
    ryu_query_result_destroy(&result);
    DB_UNGUARD(adb);
    return arr;
}

int agent_db_session_clear(AgentDB *adb, const char *session_id) {
    DB_GUARD(adb);
    char *e = cypher_esc(session_id);
    char q[512];
    snprintf(q, sizeof(q), "MATCH (m:MESSAGE {session_id:'%s'}) DELETE m", e);
    free(e);
    int _rc = db_exec(adb, q);
    DB_UNGUARD(adb);
    return _rc;
}

int agent_db_session_prune(AgentDB *adb, int64_t ttl_seconds) {
    DB_GUARD(adb);
    int64_t cutoff = (int64_t)time(NULL) - ttl_seconds;
    char q[256];
    snprintf(q, sizeof(q),
        "MATCH (s:SESSION) WHERE s.last_active < %lld "
        "WITH s MATCH (m:MESSAGE {session_id:s.id}) DELETE m "
        "WITH s DELETE s RETURN count(s)", (long long)cutoff);
    int _rc = (int)db_get_int64(adb, q, 0);
    DB_UNGUARD(adb);
    return _rc;
}

/* ========================= Tool Calls ========================= */

char *agent_db_tool_call_record(AgentDB *adb, const char *task_id,
                                 const char *tool, const char *args_json,
                                 const char *result_json, int64_t dur_ms) {
    DB_GUARD(adb);
    char id[64]; gen_id(id, sizeof(id), "tc");
    char *et = cypher_esc(tool), *ei = cypher_esc(task_id);

    /* Truncate and sanitize args/result: replace { } with ( ) to avoid
       Cypher parser treating JSON braces as property-map delimiters */
    char sa[2048] = "", sr[2048] = "";
    if (args_json) { strncpy(sa, args_json, sizeof(sa)-1); sa[sizeof(sa)-1] = 0; }
    if (result_json) { strncpy(sr, result_json, sizeof(sr)-1); sr[sizeof(sr)-1] = 0; }
    for (char *p = sa; *p; p++) { if (*p == '{') *p = '('; if (*p == '}') *p = ')'; }
    for (char *p = sr; *p; p++) { if (*p == '{') *p = '('; if (*p == '}') *p = ')'; }
    char *ta = cypher_esc(sa), *tr = cypher_esc(sr);

    char q[16384];
    snprintf(q, sizeof(q),
        "CREATE (tc:TOOL_CALL {id:'%s', tool:'%s', args_json:'%s', "
        "result_json:'%s', duration_ms:%lld, created_at:%lld})",
        id, et, ta, tr, (long long)dur_ms, (long long)time(NULL));
    free(ta); free(tr); free(et);
    if (!db_exec(adb, q)) {
        free(ei); DB_UNGUARD(adb); return NULL;
    }

    snprintf(q, sizeof(q),
        "MATCH (t:TASK {id:'%s'}), (tc:TOOL_CALL {id:'%s'}) CREATE (t)-[:CALLED]->(tc)",
        ei, id);
    free(ei);
    db_exec(adb, q);
    DB_UNGUARD(adb);
    return _strdup(id);
}

/* ========================= Files ========================= */

int agent_db_file_wrote(AgentDB *adb, const char *task_id,
                        const char *path, const char *hash, int64_t size) {
    DB_GUARD(adb);
    char *ep = cypher_esc(path), *eh = cypher_esc(hash), *et = cypher_esc(task_id);
    char q[4096];
    snprintf(q, sizeof(q),
        "MERGE (f:FILE {path:'%s'}) SET f.hash = '%s', f.size = %lld, f.created_at = %lld",
        ep, eh, (long long)size, (long long)time(NULL));
    db_exec(adb, q);
    snprintf(q, sizeof(q),
        "MATCH (t:TASK {id:'%s'}), (f:FILE {path:'%s'}) CREATE (t)-[:USED]->(f)",
        et, ep);
    free(ep); free(eh); free(et);
    int _rc = db_exec(adb, q);
    DB_UNGUARD(adb);
    return _rc;
}

int agent_db_file_read(AgentDB *adb, const char *task_id, const char *path) {
    DB_GUARD(adb);
    char *et = cypher_esc(task_id), *ep = cypher_esc(path);
    char q[4096];
    snprintf(q, sizeof(q),
        "MATCH (t:TASK {id:'%s'}), (f:FILE {path:'%s'}) CREATE (t)-[:USED]->(f)",
        et, ep);
    free(et); free(ep);
    int _rc = db_exec(adb, q);
    DB_UNGUARD(adb);
    return _rc;
}

/* ========================= Prompt Cache ========================= */

char *agent_db_cache_get(AgentDB *adb, const char *hash) {
    DB_GUARD(adb);
    char *e = cypher_esc(hash);
    char q[512];
    snprintf(q, sizeof(q),
        "MATCH (c:PROMPT_CACHE {hash:'%s'}) RETURN c.response", e);
    free(e);
    char *s = db_get_string(adb, q);
    if (!s) { DB_UNGUARD(adb); return NULL; }
    char *ret = _strdup(s);
    ryu_destroy_string(s);

    e = cypher_esc(hash);
    snprintf(q, sizeof(q),
        "MATCH (c:PROMPT_CACHE {hash:'%s'}) SET c.hit_count = c.hit_count + 1", e);
    free(e);
    db_exec(adb, q);
    DB_UNGUARD(adb);
    return ret;
}

int agent_db_cache_set(AgentDB *adb, const char *task_id,
                       const char *hash, const char *response) {
    DB_GUARD(adb);
    char *eh = cypher_esc(hash), *er = cypher_esc(response), *et = cypher_esc(task_id);
    char q[65536];
    snprintf(q, sizeof(q),
        "CREATE (c:PROMPT_CACHE {hash:'%s', response:'%s', "
        "hit_count:0, task_id:'%s', created_at:%lld})",
        eh, er, et, (long long)time(NULL));
    free(eh); free(er); free(et);
    int _rc = db_exec(adb, q);
    DB_UNGUARD(adb);
    return _rc;
}

/* ========================= Scripts ========================= */

char *agent_db_script_store(AgentDB *adb, const char *name,
                             const char *desc, const char *tool_seq) {
    DB_GUARD(adb);
    char *en = cypher_esc(name), *ed = cypher_esc(desc), *es = cypher_esc(tool_seq);
    char q[16384];
    snprintf(q, sizeof(q),
        "CREATE (s:SCRIPT {name:'%s', description:'%s', tool_sequence:'%s', "
        "success_count:0, created_at:%lld})", en, ed, es, (long long)time(NULL));
    free(en); free(ed); free(es);
    if (!db_exec(adb, q)) { DB_UNGUARD(adb); return NULL; }
    DB_UNGUARD(adb);
    return _strdup(name);
}

cJSON *agent_db_script_match(AgentDB *adb, const char *query) {
    DB_GUARD(adb);
    cJSON *arr = cJSON_CreateArray();
    if (!query || !query[0]) { DB_UNGUARD(adb); return arr; }

    char *eq = cypher_esc(query);
    char q[4096];
    snprintf(q, sizeof(q),
        "MATCH (s:SCRIPT) WHERE s.tool_sequence CONTAINS '%s' OR s.description CONTAINS '%s' "
        "RETURN s.name, s.tool_sequence, s.description, s.success_count "
        "ORDER BY s.success_count DESC LIMIT 5", eq, eq);
    free(eq);

    ryu_query_result result;
    if (ryu_connection_query(&adb->_conn, q, &result) != RyuSuccess ||
        !ryu_query_result_is_success(&result)) {
        ryu_query_result_destroy(&result);
        DB_UNGUARD(adb);
        return arr;
    }
    while (ryu_query_result_has_next(&result)) {
        ryu_flat_tuple tup;
        ryu_query_result_get_next(&result, &tup);
        char *name = dup_col(&tup, 0);
        char *seq = dup_col(&tup, 1);
        char *desc = dup_col(&tup, 2);
        char *sc = dup_col(&tup, 3);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", name ? name : "");
        cJSON_AddStringToObject(item, "tool_sequence", seq ? seq : "");
        cJSON_AddStringToObject(item, "description", desc ? desc : "");
        cJSON_AddNumberToObject(item, "success_count", sc ? atoi(sc) : 0);
        cJSON_AddItemToArray(arr, item);
        free(name); free(seq); free(desc); free(sc);
        ryu_flat_tuple_destroy(&tup);
    }
    ryu_query_result_destroy(&result);
    DB_UNGUARD(adb);
    return arr;
}

char *agent_db_task_extract_script(AgentDB *adb, const char *task_id) {
    DB_GUARD(adb);
    char *ei = cypher_esc(task_id);
    char q[1024];
    snprintf(q, sizeof(q),
        "MATCH (t:TASK {id:'%s'})-[:CALLED]->(tc:TOOL_CALL) "
        "RETURN tc.tool ORDER BY tc.created_at", ei);
    free(ei);

    /* Collect tool sequence */
    char seq[2048]; seq[0] = 0;
    int pos = 0;
    ryu_query_result result;
    if (ryu_connection_query(&adb->_conn, q, &result) != RyuSuccess ||
        !ryu_query_result_is_success(&result)) {
        ryu_query_result_destroy(&result);
        DB_UNGUARD(adb);
        return NULL;
    }
    int count = 0;
    while (ryu_query_result_has_next(&result)) {
        ryu_flat_tuple tup;
        ryu_query_result_get_next(&result, &tup);
        char *tool = dup_col(&tup, 0);
        if (tool) {
            if (count > 0) pos += snprintf(seq + pos, sizeof(seq) - pos, ",");
            pos += snprintf(seq + pos, sizeof(seq) - pos, "%s", tool);
            count++;
        }
        free(tool);
        ryu_flat_tuple_destroy(&tup);
    }
    ryu_query_result_destroy(&result);

    if (count == 0) { DB_UNGUARD(adb); return NULL; }

    /* Get task prompt for description */
    snprintf(q, sizeof(q), "MATCH (t:TASK {id:'%s'}) RETURN t.prompt", task_id);
    char *prompt = db_get_string(adb, q);
    char desc[256] = "";
    if (prompt) {
        strncpy(desc, prompt, 200); desc[200] = 0;
        ryu_destroy_string(prompt);
    }

    char script_name[128];
    snprintf(script_name, sizeof(script_name), "workflow_%s", task_id);
    char *ret = agent_db_script_store(adb, script_name, desc, seq);
    DB_UNGUARD(adb);
    return ret;
}

/* ========================= Logging ========================= */

void agent_db_log_event(AgentDB *adb, const char *type, int iter,
                        const char *stage, const char *json, int success,
                        const char *task_id) {
    DB_GUARD(adb);
    char id[64]; gen_id(id, sizeof(id), "log");
    char *et = cypher_esc(type), *es = cypher_esc(stage), *ej = cypher_esc(json ? json : "");
    char q[32768];
    snprintf(q, sizeof(q),
        "CREATE (e:LOG_EVENT {id:'%s', type:'%s', iteration:%d, "
        "stage:'%s', json_data:'%s', success:%s, created_at:%lld})",
        id, et, iter, es, ej, success ? "true" : "false", (long long)time(NULL));
    free(et); free(es); free(ej);
    db_exec(adb, q);

    /* Create RECORDED_IN relationship if task_id provided */
    if (task_id && task_id[0]) {
        char *ei = cypher_esc(task_id);
        snprintf(q, sizeof(q),
            "MATCH (e:LOG_EVENT {id:'%s'}), (t:TASK {id:'%s'}) "
            "CREATE (e)-[:RECORDED_IN]->(t)", id, ei);
        free(ei);
        db_exec(adb, q);
    }
    DB_UNGUARD(adb);
}

/* ========================= Analytics ========================= */

cJSON *agent_db_tasks_failed_ranked(AgentDB *adb) {
    DB_GUARD(adb);
    const char *q = "MATCH (t:TASK {status:'failed'}) "
        "RETURN t.id, t.prompt, t.attempts, t.last_error "
        "ORDER BY t.attempts DESC LIMIT 20";
    cJSON *arr = cJSON_CreateArray();
    ryu_query_result result;
    if (ryu_connection_query(&adb->_conn, q, &result) != RyuSuccess ||
        !ryu_query_result_is_success(&result)) {
        ryu_query_result_destroy(&result); DB_UNGUARD(adb); return arr;
    }
    while (ryu_query_result_has_next(&result)) {
        ryu_flat_tuple tup;
        ryu_query_result_get_next(&result, &tup);
        char *id = dup_col(&tup, 0), *pm = dup_col(&tup, 1);
        char *at = dup_col(&tup, 2), *err = dup_col(&tup, 3);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", id ? id : "");
        cJSON_AddStringToObject(item, "prompt", pm ? pm : "");
        cJSON_AddNumberToObject(item, "attempts", at ? atoi(at) : 0);
        cJSON_AddStringToObject(item, "error", err ? err : "");
        cJSON_AddItemToArray(arr, item);
        free(id); free(pm); free(at); free(err);
        ryu_flat_tuple_destroy(&tup);
    }
    ryu_query_result_destroy(&result);
    DB_UNGUARD(adb);
    return arr;
}

cJSON *agent_db_tools_unused(AgentDB *adb, const char **known_tools, int count) {
    DB_GUARD(adb);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        char *e = cypher_esc(known_tools[i]);
        char q[512];
        snprintf(q, sizeof(q),
            "MATCH (tc:TOOL_CALL {tool:'%s'}) RETURN count(tc)", e);
        free(e);
        int64_t c = db_get_int64(adb, q, 0);
        if (c == 0) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(known_tools[i]));
        }
    }
    DB_UNGUARD(adb);
    return arr;
}

cJSON *agent_db_file_workflow(AgentDB *adb, const char *path) {
    DB_GUARD(adb);
    char *e = cypher_esc(path);
    char q[512];
    snprintf(q, sizeof(q),
        "MATCH (t:TASK)-[:USED]->(f:FILE {path:'%s'}) "
        "RETURN t.id, t.status ORDER BY t.created_at DESC LIMIT 20", e);
    free(e);
    cJSON *arr = cJSON_CreateArray();
    ryu_query_result result;
    if (ryu_connection_query(&adb->_conn, q, &result) != RyuSuccess ||
        !ryu_query_result_is_success(&result)) {
        ryu_query_result_destroy(&result); DB_UNGUARD(adb); return arr;
    }
    while (ryu_query_result_has_next(&result)) {
        ryu_flat_tuple tup;
        ryu_query_result_get_next(&result, &tup);
        char *id = dup_col(&tup, 0), *st = dup_col(&tup, 1);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "task_id", id ? id : "");
        cJSON_AddStringToObject(item, "status", st ? st : "");
        cJSON_AddItemToArray(arr, item);
        free(id); free(st);
        ryu_flat_tuple_destroy(&tup);
    }
    ryu_query_result_destroy(&result);
    DB_UNGUARD(adb);
    return arr;
}

int agent_db_compact(AgentDB *adb, int max_age_days) {
    DB_GUARD(adb);
    int64_t cutoff = (int64_t)time(NULL) - (int64_t)max_age_days * 86400;
    int deleted = 0;

    /* Delete old LOG_EVENT nodes */
    char q[512];
    snprintf(q, sizeof(q),
        "MATCH (e:LOG_EVENT) WHERE e.created_at < %lld DELETE e RETURN count(e)",
        (long long)cutoff);
    deleted += (int)db_get_int64(adb, q, 0);

    /* Delete unused PROMPT_CACHE (hit_count=0, old) */
    snprintf(q, sizeof(q),
        "MATCH (c:PROMPT_CACHE) WHERE c.hit_count = 0 AND c.created_at < %lld "
        "DELETE c RETURN count(c)", (long long)cutoff);
    deleted += (int)db_get_int64(adb, q, 0);

    /* Delete orphaned TOOL_CALL nodes (no CALLED rel) */
    db_exec(adb,
        "MATCH (tc:TOOL_CALL) WHERE NOT EXISTS { MATCH (t:TASK)-[:CALLED]->(tc) } "
        "DELETE tc");

    DB_UNGUARD(adb);
    return deleted;
}

cJSON *agent_db_task_diff(AgentDB *adb, const char *id_a, const char *id_b) {
    DB_GUARD(adb);
    cJSON *result_obj = cJSON_CreateObject();

    /* Get tool sequences for both tasks */
    char *ea = cypher_esc(id_a), *eb = cypher_esc(id_b);
    char q[1024];

    /* Task A tools */
    snprintf(q, sizeof(q),
        "MATCH (t:TASK {id:'%s'})-[:CALLED]->(tc:TOOL_CALL) "
        "RETURN DISTINCT tc.tool", ea);
    cJSON *tools_a = cJSON_CreateArray();
    {
        ryu_query_result result;
        if (ryu_connection_query(&adb->_conn, q, &result) == RyuSuccess &&
            ryu_query_result_is_success(&result)) {
            while (ryu_query_result_has_next(&result)) {
                ryu_flat_tuple tup;
                ryu_query_result_get_next(&result, &tup);
                char *t = dup_col(&tup, 0);
                if (t) { cJSON_AddItemToArray(tools_a, cJSON_CreateString(t)); free(t); }
                ryu_flat_tuple_destroy(&tup);
            }
            ryu_query_result_destroy(&result);
        }
    }

    /* Task B tools */
    snprintf(q, sizeof(q),
        "MATCH (t:TASK {id:'%s'})-[:CALLED]->(tc:TOOL_CALL) "
        "RETURN DISTINCT tc.tool", eb);
    free(ea); free(eb);
    cJSON *tools_b = cJSON_CreateArray();
    {
        ryu_query_result result;
        if (ryu_connection_query(&adb->_conn, q, &result) == RyuSuccess &&
            ryu_query_result_is_success(&result)) {
            while (ryu_query_result_has_next(&result)) {
                ryu_flat_tuple tup;
                ryu_query_result_get_next(&result, &tup);
                char *t = dup_col(&tup, 0);
                if (t) { cJSON_AddItemToArray(tools_b, cJSON_CreateString(t)); free(t); }
                ryu_flat_tuple_destroy(&tup);
            }
            ryu_query_result_destroy(&result);
        }
    }

    cJSON_AddItemToObject(result_obj, "tools_a", tools_a);
    cJSON_AddItemToObject(result_obj, "tools_b", tools_b);
    DB_UNGUARD(adb);
    return result_obj;
}

/* ========================= Graph Stats ========================= */

int64_t agent_db_node_count(AgentDB *adb) {
    DB_GUARD(adb);
    int64_t _rc = db_get_int64(adb, "MATCH (n) RETURN count(n)", 0);
    DB_UNGUARD(adb);
    return _rc;
}
int64_t agent_db_rel_count(AgentDB *adb) {
    DB_GUARD(adb);
    int64_t _rc = db_get_int64(adb, "MATCH ()-[r]->() RETURN count(r)", 0);
    DB_UNGUARD(adb);
    return _rc;
}

/* ========================= Migration ========================= */

int agent_db_needs_migration(AgentDB *adb) {
    return agent_db_node_count(adb) == 0 ? 1 : 0;
}

int agent_db_migrate_tasks_json(AgentDB *adb, const char *path) {
    char *data = read_file_contents(path);
    if (!data) return 0;
    cJSON *root = cJSON_Parse(data); free(data);
    if (!root) return 0;
    cJSON *arr = cJSON_GetObjectItem(root, "tasks");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); return 0; }

    int count = cJSON_GetArraySize(arr), imported = 0;
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *f;
        char id[32]="",prompt[4096]="",status[16]="pending",result[2048]="";
        char source[16]="cli",last_error[1024]="",plan[4096]="",state[8192]="";
        long long chat_id=0; int priority=5,attempts=0,max_attempts=3;
        long long created_at=0,updated_at=0;

        f=cJSON_GetObjectItem(item,"id"); if(cJSON_IsString(f)) strncpy(id,f->valuestring,31);
        f=cJSON_GetObjectItem(item,"prompt"); if(cJSON_IsString(f)) strncpy(prompt,f->valuestring,4095);
        f=cJSON_GetObjectItem(item,"status"); if(cJSON_IsString(f)) strncpy(status,f->valuestring,15);
        f=cJSON_GetObjectItem(item,"result"); if(cJSON_IsString(f)) strncpy(result,f->valuestring,2047);
        f=cJSON_GetObjectItem(item,"source"); if(cJSON_IsString(f)) strncpy(source,f->valuestring,15);
        f=cJSON_GetObjectItem(item,"last_error"); if(cJSON_IsString(f)) strncpy(last_error,f->valuestring,1023);
        f=cJSON_GetObjectItem(item,"plan"); if(cJSON_IsString(f)) strncpy(plan,f->valuestring,4095);
        f=cJSON_GetObjectItem(item,"state"); if(cJSON_IsString(f)) strncpy(state,f->valuestring,8191);
        f=cJSON_GetObjectItem(item,"chat_id"); if(cJSON_IsNumber(f)) chat_id=(long long)f->valuedouble;
        f=cJSON_GetObjectItem(item,"priority"); if(cJSON_IsNumber(f)) priority=f->valueint;
        f=cJSON_GetObjectItem(item,"attempts"); if(cJSON_IsNumber(f)) attempts=f->valueint;
        f=cJSON_GetObjectItem(item,"max_attempts"); if(cJSON_IsNumber(f)) max_attempts=f->valueint;
        f=cJSON_GetObjectItem(item,"created_at"); if(cJSON_IsNumber(f)) created_at=(long long)f->valuedouble;
        f=cJSON_GetObjectItem(item,"updated_at"); if(cJSON_IsNumber(f)) updated_at=(long long)f->valuedouble;
        if(!id[0]) continue;

        char *ei=cypher_esc(id),*ep=cypher_esc(prompt),*est=cypher_esc(status);
        char *er=cypher_esc(result),*es=cypher_esc(source),*epl=cypher_esc(plan);
        char *est2=cypher_esc(state),*ele=cypher_esc(last_error);
        char q[32768];
        snprintf(q,sizeof(q),
            "CREATE (t:TASK {id:'%s',name:'',prompt:'%s',status:'%s',"
            "result:'%s',source:'%s',chat_id:%lld,priority:%d,"
            "attempts:%d,max_attempts:%d,plan:'%s',state:'%s',"
            "last_error:'%s',created_at:%lld,updated_at:%lld})",
            ei,ep,est,er,es,chat_id,priority,attempts,max_attempts,
            epl,est2,ele,created_at,updated_at);
        free(ei);free(ep);free(est);free(er);free(es);free(epl);free(est2);free(ele);
        if(db_exec(adb,q)) imported++;
    }
    cJSON_Delete(root);
    printf("[DB] Migrated %d/%d tasks\n", imported, count);
    return imported;
}

int agent_db_migrate_memory_json(AgentDB *adb, const char *path) {
    char *data = read_file_contents(path);
    if (!data) return 0;
    cJSON *root = cJSON_Parse(data); free(data);
    if (!root) return 0;
    int imported = 0;
    cJSON *sc;
    sc=cJSON_GetObjectItem(root,"step_count");
    if(cJSON_IsNumber(sc)){char b[32];snprintf(b,32,"%d",sc->valueint);agent_db_memory_set_scalar(adb,"step_count",b);imported++;}
    sc=cJSON_GetObjectItem(root,"total_messages");
    if(cJSON_IsNumber(sc)){char b[32];snprintf(b,32,"%lld",(long long)sc->valuedouble);agent_db_memory_set_scalar(adb,"total_messages",b);imported++;}
    sc=cJSON_GetObjectItem(root,"summary");
    if(cJSON_IsString(sc)){agent_db_memory_set_scalar(adb,"summary",sc->valuestring);imported++;}
    cJSON *files=cJSON_GetObjectItem(root,"files_created");
    if(cJSON_IsArray(files)){int n=cJSON_GetArraySize(files);for(int i=0;i<n;i++){cJSON *it=cJSON_GetArrayItem(files,i);if(cJSON_IsString(it)){agent_db_memory_add(adb,"file",it->valuestring,it->valuestring);imported++;}}}
    cJSON *errors=cJSON_GetObjectItem(root,"known_errors");
    if(cJSON_IsArray(errors)){int n=cJSON_GetArraySize(errors);for(int i=0;i<n;i++){cJSON *it=cJSON_GetArrayItem(errors,i);if(cJSON_IsString(it)){agent_db_memory_add(adb,"error","",it->valuestring);imported++;}}}
    cJSON *goals=cJSON_GetObjectItem(root,"goals");
    if(cJSON_IsArray(goals)){int n=cJSON_GetArraySize(goals);for(int i=0;i<n;i++){cJSON *g=cJSON_GetArrayItem(goals,i);cJSON *t=cJSON_GetObjectItem(g,"text");if(cJSON_IsString(t)){agent_db_goal_create(adb,t->valuestring);imported++;}}}
    cJSON_Delete(root);
    printf("[DB] Migrated %d memory records\n", imported);
    return imported;
}

int agent_db_migrate_cache_json(AgentDB *adb, const char *path) {
    char *data = read_file_contents(path);
    if (!data) return 0;
    cJSON *root = cJSON_Parse(data); free(data);
    if (!root||!cJSON_IsArray(root)) { if(root) cJSON_Delete(root); return 0; }
    int n=cJSON_GetArraySize(root), imported=0;
    for(int i=0;i<n;i++){
        cJSON *entry=cJSON_GetArrayItem(root,i);
        cJSON *h=cJSON_GetObjectItem(entry,"hash"),*r=cJSON_GetObjectItem(entry,"response");
        if(cJSON_IsString(h)&&cJSON_IsString(r)){
            if(agent_db_cache_set(adb,"migrated",h->valuestring,r->valuestring)) imported++;
        }
    }
    cJSON_Delete(root);
    printf("[DB] Migrated %d/%d cache entries\n", imported, n);
    return imported;
}
