#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "llm.h"
#include "state.h"
#include "agent_db.h"
#include "cJSON.h"
#include "tasks.h"
#include "agent_config.h"
#include "ryu.h"

#pragma comment(lib, "winhttp.lib")

#define INITIAL_BUF 4096
#define MAX_RESPONSE_SIZE (256 * 1024)
#define MAX_JSON_SIZE    (64 * 1024)

extern AgentDB g_adb;

static char g_endpoint[256];
static char g_embed_endpoint[256];
static char g_model[64];
static char g_apikey[256];
static int  g_timeout_ms = 30000;

/* ========================= Util ========================= */

static char *str_dup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *d = (char *)malloc(len + 1);
    if (d) memcpy(d, s, len + 1);
    return d;
}

/* ========================= Prompt Cache ========================= */

static int  g_cache_enabled = 0;
static int  g_last_cache_hit = 0;
static int  g_cache_hits = 0;
static int  g_cache_misses = 0;

/* FNV-1a 64-bit */
unsigned long long llm_hash_prompt(const char *prompt) {
    if (!prompt) return 0;
    unsigned long long h = 14695981039346656037ULL;
    while (*prompt) {
        h ^= (unsigned char)*prompt++;
        h *= 1099511628211ULL;
    }
    return h;
}

void llm_hash_to_hex(unsigned long long hash, char *out, size_t out_size) {
    if (!out || out_size < 17) return;
    snprintf(out, out_size, "%016llx", hash);
}

void llm_cache_init(int enabled, const char *cache_path) {
    (void)cache_path;
    g_cache_enabled = enabled;
    g_cache_hits = 0;
    g_cache_misses = 0;
    g_last_cache_hit = 0;
}

int llm_cache_load(void) {
    /* Graph cache is always loaded — no file import needed in v4 */
    return 0;
}

int llm_cache_save(void) {
    /* Graph cache persists natively — no file export needed in v4 */
    return 1;
}

void llm_cache_stats(int *hits, int *misses, int *entries) {
    if (hits) *hits = g_cache_hits;
    if (misses) *misses = g_cache_misses;
    if (entries) *entries = -1; /* count lives in graph */
}

int llm_last_cache_hit(void) {
    return g_last_cache_hit;
}

static void trim(char *s) {
    if (!s) return;
    while (isspace((unsigned char)*s)) memmove(s, s + 1, strlen(s));
    int len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = 0;
}

/* ========================= JSON String Escaping ========================= */

char *llm_escape_json_string(const char *s) {
    if (!s) return str_dup("");

    size_t len = strlen(s);
    /* Worst case: every char needs escaping */
    char *out = (char *)malloc(len * 6 + 1);
    if (!out) return NULL;

    char *p = out;
    for (const char *c = s; *c; c++) {
        switch (*c) {
            case '"':  *p++ = '\\'; *p++ = '"';  break;
            case '\\': *p++ = '\\'; *p++ = '\\'; break;
            case '\b': *p++ = '\\'; *p++ = 'b';  break;
            case '\f': *p++ = '\\'; *p++ = 'f';  break;
            case '\n': *p++ = '\\'; *p++ = 'n';  break;
            case '\r': *p++ = '\\'; *p++ = 'r';  break;
            case '\t': *p++ = '\\'; *p++ = 't';  break;
            default:
                if ((unsigned char)*c < 0x20) {
                    p += sprintf(p, "\\u%04x", (unsigned char)*c);
                } else {
                    *p++ = *c;
                }
                break;
        }
    }
    *p = 0;
    return out;
}

/* ========================= Token Tracking (Phase 11) ========================= */

static LLMTokenUsage g_token_usage = {0, 0, 0};

LLMTokenUsage llm_token_usage(void) {
    return g_token_usage;
}

static void track_usage(const char *response_body) {
    if (!response_body) return;
    cJSON *root = cJSON_Parse(response_body);
    if (!root) return;
    cJSON *usage = cJSON_GetObjectItem(root, "usage");
    if (usage) {
        cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
        cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
        if (pt && cJSON_IsNumber(pt)) g_token_usage.total_prompt_tokens += pt->valueint;
        if (ct && cJSON_IsNumber(ct)) g_token_usage.total_completion_tokens += ct->valueint;
        g_token_usage.request_count++;
    }
    cJSON_Delete(root);
}

/* ========================= Init ========================= */

void llm_init(const char *endpoint, const char *model, const char *apikey, int timeout_ms) {
    strncpy(g_endpoint, endpoint, sizeof(g_endpoint) - 1);
    g_endpoint[sizeof(g_endpoint) - 1] = 0;
    strncpy(g_model, model, sizeof(g_model) - 1);
    g_model[sizeof(g_model) - 1] = 0;
    strncpy(g_apikey, apikey, sizeof(g_apikey) - 1);
    g_apikey[sizeof(g_apikey) - 1] = 0;
    g_timeout_ms = timeout_ms;

    /* Derive embeddings endpoint from chat endpoint */
    g_embed_endpoint[0] = 0;
    const char *cc = strstr(g_endpoint, "/chat/completions");
    if (cc) {
        size_t prefix_len = cc - g_endpoint;
        memcpy(g_embed_endpoint, g_endpoint, prefix_len);
        strcpy(g_embed_endpoint + prefix_len, "/embeddings");
    }
}

/* ========================= HTTP ========================= */

static int parse_url(const char *url, wchar_t *host, wchar_t *path, INTERNET_PORT *port, int *secure) {
    URL_COMPONENTSW uc = {0};
    wchar_t wurl[1024];

    mbstowcs(wurl, url, 1024);

    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = -1;
    uc.dwUrlPathLength = -1;

    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return 0;

    wcsncpy(host, uc.lpszHostName, uc.dwHostNameLength);
    host[uc.dwHostNameLength] = 0;

    wcsncpy(path, uc.lpszUrlPath, uc.dwUrlPathLength);
    path[uc.dwUrlPathLength] = 0;

    *port = uc.nPort;
    *secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    return 1;
}

static char *http_post(const char *url, const char *payload) {
    wchar_t host[256], path[512];
    INTERNET_PORT port;
    int secure;

    if (!parse_url(url, host, path, &port, &secure)) return NULL;

    HINTERNET hSession = WinHttpOpen(L"BashAgent/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession) return NULL;

    WinHttpSetTimeouts(hSession, g_timeout_ms, g_timeout_ms, g_timeout_ms, g_timeout_ms);

    HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return NULL;
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"POST",
        path,
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        secure ? WINHTTP_FLAG_SECURE : 0
    );

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return NULL;
    }

    char headers_a[512];
    snprintf(headers_a, sizeof(headers_a),
        "Content-Type: application/json\r\nAuthorization: Bearer %s\r\n",
        g_apikey
    );

    wchar_t headers_w[512];
    mbstowcs(headers_w, headers_a, 512);

    int payload_len = (int)strlen(payload);

    if (!WinHttpSendRequest(hRequest,
        headers_w, -1,
        (LPVOID)payload, payload_len,
        payload_len, 0)) {
        goto cleanup;
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        goto cleanup;
    }

    /* Read status code */
    DWORD status_code = 0;
    DWORD status_len = sizeof(status_code);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        NULL, &status_code, &status_len, NULL);

    int buf_size = INITIAL_BUF;
    char *buffer = (char *)malloc(buf_size);
    if (!buffer) goto cleanup;
    int total = 0;

    while (1) {
        DWORD size = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &size)) break;
        if (size == 0) break;

        if (total + (int)size >= buf_size) {
            buf_size *= 2;
            if (buf_size > MAX_RESPONSE_SIZE) break;
            buffer = (char *)realloc(buffer, buf_size);
            if (!buffer) goto cleanup;
        }

        DWORD downloaded = 0;
        if (!WinHttpReadData(hRequest, buffer + total, size, &downloaded)) break;
        total += downloaded;
    }

    buffer[total] = 0;
    return buffer;

cleanup:
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return NULL;
}

/* ========================= LLM Chat ========================= */

char *llm_chat(const char *prompt, int max_retries) {
    /* Cache check — graph-backed */
    unsigned long long hash = llm_hash_prompt(prompt);
    if (g_cache_enabled) {
        char hex[17];
        llm_hash_to_hex(hash, hex, sizeof(hex));
        char *cached = agent_db_cache_get(&g_adb, hex);
        if (cached) {
            g_last_cache_hit = 1;
            g_cache_hits++;
            return cached;
        }
    }
    g_last_cache_hit = 0;
    g_cache_misses++;

    char *escaped = llm_escape_json_string(prompt);
    if (!escaped) return NULL;

    size_t payload_size = strlen(escaped) + 512;
    char *payload = (char *)malloc(payload_size);
    if (!payload) {
        free(escaped);
        return NULL;
    }

    snprintf(payload, payload_size,
        "{\"model\":\"%s\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],"
        "\"temperature\":0.3,"
        "\"max_tokens\":4096}",
        g_model, escaped
    );
    free(escaped);

    for (int i = 0; i < max_retries; i++) {
        char *resp = http_post(g_endpoint, payload);
        if (!resp) {
            fprintf(stderr, "[LLM] Transport error, retry %d/%d\n", i + 1, max_retries);
            Sleep(1000 * (i + 1));
            continue;
        }

        if (strlen(resp) > MAX_RESPONSE_SIZE) {
            free(resp);
            continue;
        }

        cJSON *root = cJSON_Parse(resp);
        if (!root) {
            free(resp);
            continue;
        }

        cJSON *choices = cJSON_GetObjectItem(root, "choices");
        cJSON *first = cJSON_GetArrayItem(choices, 0);
        cJSON *msg = cJSON_GetObjectItem(first, "message");
        cJSON *content = cJSON_GetObjectItem(msg, "content");

        if (!cJSON_IsString(content)) {
            cJSON_Delete(root);
            free(resp);
            continue;
        }

        char *result = str_dup(content->valuestring);
        cJSON_Delete(root);
        track_usage(resp);
        free(resp);
        free(payload);

        /* Store in graph cache */
        if (g_cache_enabled && result) {
            char hex[17];
            llm_hash_to_hex(hash, hex, sizeof(hex));
            agent_db_cache_set(&g_adb, "", hex, result);
        }

        return result;
    }

    free(payload);
    return NULL;
}

/* ========================= Multi-Turn Chat ========================= */

char *llm_chat_multi(ChatMessage *msgs, int count, int max_retries) {
    if (!msgs || count <= 0) return NULL;

    /* Build messages JSON array */
    /* Each message: {"role":"xxx","content":"escaped_content"} */
    /* Worst case: each content char becomes 6 chars when escaped */
    size_t buf_size = 512;  /* overhead */
    for (int i = 0; i < count; i++) {
        buf_size += 64 + (msgs[i].content ? strlen(msgs[i].content) * 6 + 2 : 4);
    }
    buf_size += 256;  /* model, temperature, etc. */

    char *payload = (char *)malloc(buf_size);
    if (!payload) return NULL;

    int pos = snprintf(payload, buf_size,
        "{\"model\":\"%s\",\"messages\":[", g_model);

    for (int i = 0; i < count; i++) {
        if (i > 0) payload[pos++] = ',';

        if (!msgs[i].content) {
            pos += snprintf(payload + pos, buf_size - pos,
                "{\"role\":\"%s\",\"content\":\"\"}", msgs[i].role);
        } else {
            char *escaped = llm_escape_json_string(msgs[i].content);
            if (!escaped) {
                free(payload);
                return NULL;
            }
            pos += snprintf(payload + pos, buf_size - pos,
                "{\"role\":\"%s\",\"content\":\"%s\"}", msgs[i].role, escaped);
            free(escaped);
        }
    }

    pos += snprintf(payload + pos, buf_size - pos,
        "],\"temperature\":0.3,\"max_tokens\":4096}");

    for (int i = 0; i < max_retries; i++) {
        char *resp = http_post(g_endpoint, payload);
        if (!resp) {
            fprintf(stderr, "[LLM] Transport error (multi-turn), retry %d/%d\n", i + 1, max_retries);
            Sleep(1000 * (i + 1));
            continue;
        }

        if (strlen(resp) > MAX_RESPONSE_SIZE) {
            free(resp);
            continue;
        }

        cJSON *root = cJSON_Parse(resp);
        if (!root) {
            free(resp);
            continue;
        }

        cJSON *choices = cJSON_GetObjectItem(root, "choices");
        cJSON *first = cJSON_GetArrayItem(choices, 0);
        cJSON *msg = cJSON_GetObjectItem(first, "message");
        cJSON *content = cJSON_GetObjectItem(msg, "content");

        if (!cJSON_IsString(content)) {
            cJSON_Delete(root);
            free(resp);
            continue;
        }

        char *result = str_dup(content->valuestring);
        cJSON_Delete(root);
        free(resp);
        free(payload);
        return result;
    }

    free(payload);
    return NULL;
}

/* ========================= JSON Extraction ========================= */

static char *extract_between(const char *src, const char *start, const char *end) {
    char *s = strstr(src, start);
    if (!s) return NULL;
    s += strlen(start);

    char *e = strstr(s, end);
    if (!e) return NULL;

    size_t len = e - s;
    if (len > MAX_JSON_SIZE) return NULL;

    char *out = (char *)malloc(len + 1);
    memcpy(out, s, len);
    out[len] = 0;
    return out;
}

static char *extract_brace_block(const char *src) {
    const char *start = strchr(src, '{');
    if (!start) return NULL;

    int depth = 0;
    const char *p = start;
    int in_string = 0;

    while (*p) {
        if (*p == '"' && (p == start || *(p - 1) != '\\')) {
            in_string = !in_string;
        }
        if (!in_string) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
        }

        if (depth == 0) {
            size_t len = p - start + 1;
            if (len > MAX_JSON_SIZE) return NULL;

            char *out = (char *)malloc(len + 1);
            memcpy(out, start, len);
            out[len] = 0;
            return out;
        }
        p++;
    }
    return NULL;
}

static void strip_markdown(char *s) {
    char *p;
    /* Remove ```json and ``` markers */
    while ((p = strstr(s, "```"))) {
        /* Find end of line after ``` */
        char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        /* Also skip "json" or "JSON" after ``` */
        char *after = p + 3;
        while (after < eol && (*after == 'j' || *after == 's' || *after == 'o' ||
               *after == 'n' || *after == 'J' || *after == 'S' || *after == 'O' ||
               *after == 'N' || *after == '\r' || *after == '\n')) after++;
        if (*after == '\n') after++;
        memmove(p, after, strlen(after) + 1);
    }
}

static void strip_trailing_commas(char *s) {
    /* Remove trailing commas before } or ] */
    int len = (int)strlen(s);
    for (int i = 0; i < len; i++) {
        if (s[i] == ',' && i + 1 < len) {
            int j = i + 1;
            while (j < len && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) j++;
            if (j < len && (s[j] == '}' || s[j] == ']')) {
                s[i] = ' ';
            }
        }
    }
}

char *llm_extract_json(const char *response) {
    if (!response) return NULL;
    if (strlen(response) > MAX_RESPONSE_SIZE) return NULL;

    char *json = NULL;

    /* PASS 1: <JSON>...</JSON> tags */
    json = extract_between(response, "<JSON>", "</JSON>");
    if (json) {
        trim(json);
        cJSON *test = cJSON_Parse(json);
        if (test) {
            cJSON_Delete(test);
            return json;
        }
        free(json);
    }

    /* PASS 2: Largest {...} block with markdown stripping */
    json = extract_brace_block(response);
    if (json) {
        strip_markdown(json);
        strip_trailing_commas(json);
        trim(json);

        cJSON *test = cJSON_Parse(json);
        if (test) {
            cJSON_Delete(test);
            return json;
        }
        free(json);
    }

    return NULL;
}

/* ========================= Embeddings ========================= */

float *llm_embed(const char *text, int *out_dim) {
    if (out_dim) *out_dim = 0;
    if (!g_embed_endpoint[0]) return NULL;
    if (!text || !text[0]) return NULL;

    char *escaped = llm_escape_json_string(text);
    if (!escaped) return NULL;

    size_t payload_size = strlen(escaped) + 256;
    char *payload = (char *)malloc(payload_size);
    if (!payload) { free(escaped); return NULL; }

    snprintf(payload, payload_size,
        "{\"model\":\"%s\",\"input\":\"%s\"}", g_model, escaped);
    free(escaped);

    char *resp = http_post(g_embed_endpoint, payload);
    free(payload);
    if (!resp) return NULL;

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) return NULL;

    /* Parse: { "data": [{ "embedding": [0.1, 0.2, ...] }] } */
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *first = cJSON_GetArrayItem(data, 0);
    cJSON *emb = first ? cJSON_GetObjectItem(first, "embedding") : NULL;
    if (!cJSON_IsArray(emb)) {
        cJSON_Delete(root);
        return NULL;
    }

    int dim = cJSON_GetArraySize(emb);
    float *vec = (float *)malloc(dim * sizeof(float));
    if (!vec) { cJSON_Delete(root); return NULL; }

    for (int i = 0; i < dim; i++) {
        cJSON *item = cJSON_GetArrayItem(emb, i);
        vec[i] = (float)(cJSON_IsNumber(item) ? item->valuedouble : 0.0);
    }

    cJSON_Delete(root);
    if (out_dim) *out_dim = dim;
    return vec;
}

/* ========================= Multi-Provider (Phase 11) ========================= */

#define MAX_PROVIDERS 8
typedef struct {
    char name[32];
    char endpoint[256];
    char apikey[256];
    char model[64];
} NamedProvider;

static NamedProvider g_providers[MAX_PROVIDERS];
static int g_provider_count = 0;

/* Role→provider routing */
static char g_role_provider_map[8][64];  /* role name → provider name */
static int g_role_provider_count = 0;

void llm_add_provider(const char *name, const char *endpoint,
                      const char *api_key, const char *model) {
    if (!name || g_provider_count >= MAX_PROVIDERS) return;
    NamedProvider *p = &g_providers[g_provider_count];
    strncpy(p->name, name, sizeof(p->name) - 1);
    strncpy(p->endpoint, endpoint ? endpoint : "", sizeof(p->endpoint) - 1);
    strncpy(p->apikey, api_key ? api_key : "", sizeof(p->apikey) - 1);
    strncpy(p->model, model ? model : "", sizeof(p->model) - 1);
    g_provider_count++;
}

void llm_set_role_provider(const char *role, const char *provider_name) {
    if (!role || g_role_provider_count >= 8) return;
    strncpy(g_role_provider_map[g_role_provider_count], role, 31);
    strncat(g_role_provider_map[g_role_provider_count], ":", 1);
    if (provider_name)
        strncat(g_role_provider_map[g_role_provider_count], provider_name, 30);
    g_role_provider_count++;
}

static NamedProvider *find_provider(const char *name) {
    for (int i = 0; i < g_provider_count; i++) {
        if (strcmp(g_providers[i].name, name) == 0) return &g_providers[i];
    }
    return NULL;
}

/* ========================= SSE Streaming (Phase 11) ========================= */

/* Parse SSE data from WinHTTP response. Yields tokens from "data: ..." lines.
   Returns the full accumulated content. */
static char *parse_sse_response(const char *raw_response, llm_stream_fn on_token, void *user_data) {
    if (!raw_response) return NULL;

    size_t buf_cap = 4096;
    char *content = (char *)malloc(buf_cap);
    if (!content) return NULL;
    content[0] = 0;
    size_t content_len = 0;

    const char *p = raw_response;
    while (*p) {
        /* Find "data: " prefix */
        const char *data_line = strstr(p, "data: ");
        if (!data_line) break;
        data_line += 6; /* skip "data: " */

        /* Check for [DONE] */
        if (strncmp(data_line, "[DONE]", 6) == 0) break;

        /* Find end of line */
        const char *eol = strchr(data_line, '\n');
        size_t line_len;
        if (eol) {
            line_len = eol - data_line;
        } else {
            line_len = strlen(data_line);
        }

        /* Parse JSON: {"choices":[{"delta":{"content":"token"}}]} */
        char *line = (char *)malloc(line_len + 1);
        if (!line) break;
        memcpy(line, data_line, line_len);
        line[line_len] = 0;

        cJSON *root = cJSON_Parse(line);
        if (root) {
            cJSON *choices = cJSON_GetObjectItem(root, "choices");
            cJSON *first = cJSON_GetArrayItem(choices, 0);
            cJSON *delta = first ? cJSON_GetObjectItem(first, "delta") : NULL;
            cJSON *ct = delta ? cJSON_GetObjectItem(delta, "content") : NULL;

            if (cJSON_IsString(ct) && ct->valuestring[0]) {
                const char *token = ct->valuestring;
                size_t tlen = strlen(token);

                /* Append to content buffer */
                if (content_len + tlen + 1 >= buf_cap) {
                    buf_cap *= 2;
                    content = (char *)realloc(content, buf_cap);
                    if (!content) { free(line); cJSON_Delete(root); return NULL; }
                }
                memcpy(content + content_len, token, tlen);
                content_len += tlen;
                content[content_len] = 0;

                /* Callback */
                if (on_token) on_token(token, user_data);
            }
            cJSON_Delete(root);
        }
        free(line);

        p = data_line + line_len;
        if (*p == '\n' || *p == '\r') p++;
    }

    return content;
}

char *llm_chat_stream(const char *prompt, int max_retries,
                      llm_stream_fn on_token, void *user_data) {
    /* Build streaming payload */
    char *escaped = llm_escape_json_string(prompt);
    if (!escaped) return NULL;

    size_t payload_size = strlen(escaped) + 512;
    char *payload = (char *)malloc(payload_size);
    if (!payload) { free(escaped); return NULL; }

    snprintf(payload, payload_size,
        "{\"model\":\"%s\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],"
        "\"temperature\":0.3,"
        "\"max_tokens\":4096,"
        "\"stream\":true}",
        g_model, escaped);
    free(escaped);

    for (int i = 0; i < max_retries; i++) {
        char *resp = http_post(g_endpoint, payload);
        if (!resp) {
            fprintf(stderr, "[LLM] Stream transport error, retry %d/%d\n", i + 1, max_retries);
            Sleep(1000 * (i + 1));
            continue;
        }

        /* Try SSE parsing */
        char *result = parse_sse_response(resp, on_token, user_data);
        if (result && result[0]) {
            track_usage(resp);
            free(resp);
            free(payload);
            return result;
        }

        /* SSE parse failed — fallback: treat entire response as non-streaming */
        free(result);
        cJSON *root = cJSON_Parse(resp);
        if (root) {
            cJSON *choices = cJSON_GetObjectItem(root, "choices");
            cJSON *first = cJSON_GetArrayItem(choices, 0);
            cJSON *msg = cJSON_GetObjectItem(first, "message");
            cJSON *content = cJSON_GetObjectItem(msg, "content");
            if (cJSON_IsString(content)) {
                char *r = str_dup(content->valuestring);
                track_usage(resp);
                cJSON_Delete(root);
                free(resp);
                free(payload);
                return r;
            }
            cJSON_Delete(root);
        }
        free(resp);
    }

    free(payload);
    return NULL;
}

/* ========================= Role-Based Pipeline (Phase 6) ========================= */

/* External config — defined in main.c */
extern AgentConfig g_agent_cfg;

char *llm_chat_with_role(const char *role_system, const char *tools_list,
                         const char *user_content, int max_retries) {
    if (!role_system || !user_content) return NULL;

    /* Build 2-message conversation: system + user */
    ChatMessage msgs[2];
    memset(msgs, 0, sizeof(msgs));

    /* System message: role prompt + tools */
    size_t sys_size = strlen(role_system) + (tools_list ? strlen(tools_list) : 0) + 256;
    char *sys_buf = (char *)malloc(sys_size);
    if (!sys_buf) return NULL;
    int spos = 0;
    spos += snprintf(sys_buf + spos, sys_size - spos, "%s\n", role_system);
    if (tools_list && tools_list[0]) {
        spos += snprintf(sys_buf + spos, sys_size - spos,
            "\nAVAILABLE TOOLS:\n%s\n", tools_list);
    }
    strncpy(msgs[0].role, "system", sizeof(msgs[0].role) - 1);
    msgs[0].content = sys_buf;

    strncpy(msgs[1].role, "user", sizeof(msgs[1].role) - 1);
    msgs[1].content = (char *)user_content;  /* not owned */

    char *result = llm_chat_multi(msgs, 2, max_retries);

    free(sys_buf);
    return result;
}

char *llm_build_context(const char *task_prompt, const char *task_id) {
    if (!task_prompt) return str_dup("");

    size_t buf_size = MAX_CONTEXT_CHARS;
    char *buf = (char *)malloc(buf_size);
    if (!buf) return NULL;
    int pos = 0;

    /* 1. Current goal */
    {
        agent_db_lock();
        ryu_query_result result;
        const char *q = "MATCH (g:GOAL) RETURN g.text ORDER BY g.id DESC LIMIT 1";
        ryu_state st = ryu_connection_query(&g_adb._conn, q, &result);
        if (st == RyuSuccess && ryu_query_result_is_success(&result)) {
            while (ryu_query_result_has_next(&result)) {
                ryu_flat_tuple tup;
                ryu_query_result_get_next(&result, &tup);
                char *txt = NULL;
                ryu_value val;
                ryu_flat_tuple_get_value(&tup, 0, &val);
                ryu_value_get_string(&val, &txt);
                if (txt && txt[0]) {
                    pos += snprintf(buf + pos, buf_size - pos,
                        "Current goal: %s\n", txt);
                    ryu_destroy_string(txt);
                }
                ryu_value_destroy(&val);
                ryu_flat_tuple_destroy(&tup);
            }
        }
        ryu_query_result_destroy(&result);
        agent_db_unlock();
    }

    /* 2. Semantic memory: top 3 relevant memories */
    {
        int dim = 0;
        float *qvec = llm_embed(task_prompt, &dim);
        if (qvec) {
            agent_db_lock();
            cJSON *mems = agent_db_memory_search(&g_adb, qvec, dim, 3);
            agent_db_unlock();
            int nm = cJSON_GetArraySize(mems);
            if (nm > 0) {
                pos += snprintf(buf + pos, buf_size - pos, "Relevant memories:\n");
                for (int i = 0; i < nm; i++) {
                    cJSON *m = cJSON_GetArrayItem(mems, i);
                    cJSON *mc = cJSON_GetObjectItem(m, "content");
                    if (cJSON_IsString(mc) && mc->valuestring[0]) {
                        pos += snprintf(buf + pos, buf_size - pos,
                            "- %s\n", mc->valuestring);
                    }
                }
            }
            cJSON_Delete(mems);
            free(qvec);
        }
    }

    /* 3. Script match: top 1 */
    {
        agent_db_lock();
        cJSON *scripts = agent_db_script_match(&g_adb, task_prompt);
        agent_db_unlock();
        if (cJSON_GetArraySize(scripts) > 0) {
            cJSON *s = cJSON_GetArrayItem(scripts, 0);
            cJSON *sn = cJSON_GetObjectItem(s, "name");
            cJSON *sseq = cJSON_GetObjectItem(s, "tool_sequence");
            if (cJSON_IsString(sn) && cJSON_IsString(sseq)) {
                pos += snprintf(buf + pos, buf_size - pos,
                    "Past workflow '%s': %s\n", sn->valuestring, sseq->valuestring);
            }
        }
        cJSON_Delete(scripts);
    }

    /* 4. Recent related tasks (last 5) */
    {
        agent_db_lock();
        ryu_query_result result;
        const char *q = "MATCH (t:TASK) WHERE t.status <> 'running' "
                        "RETURN t.prompt, t.status, t.result "
                        "ORDER BY t.updated_at DESC LIMIT 5";
        ryu_state st = ryu_connection_query(&g_adb._conn, q, &result);
        if (st == RyuSuccess && ryu_query_result_is_success(&result)) {
            int count = 0;
            while (ryu_query_result_has_next(&result) && count < 5) {
                ryu_flat_tuple tup;
                ryu_query_result_get_next(&result, &tup);
                char *prompt_s = NULL, *status_s = NULL;
                ryu_value v0, v1;
                ryu_flat_tuple_get_value(&tup, 0, &v0);
                ryu_flat_tuple_get_value(&tup, 1, &v1);
                ryu_value_get_string(&v0, &prompt_s);
                ryu_value_get_string(&v1, &status_s);
                if (prompt_s && prompt_s[0] && status_s) {
                    pos += snprintf(buf + pos, buf_size - pos,
                        "Recent task [%s]: %.200s\n", status_s, prompt_s);
                    count++;
                }
                if (prompt_s) ryu_destroy_string(prompt_s);
                if (status_s) ryu_destroy_string(status_s);
                ryu_value_destroy(&v0);
                ryu_value_destroy(&v1);
                ryu_flat_tuple_destroy(&tup);
            }
        }
        ryu_query_result_destroy(&result);
        agent_db_unlock();
    }

    /* Truncate at MAX_CONTEXT_CHARS */
    if (pos >= (int)buf_size - 1) {
        buf[buf_size - 1] = 0;
    }

    return buf;
}

cJSON *llm_parse_tool_call(const char *response) {
    if (!response) return NULL;

    /* Strategy 1: Look for <tool_call name="..." args="{...}"/> tags */
    const char *tag = strstr(response, "<tool_call ");
    if (tag) {
        const char *name_start = strstr(tag, "name=\"");
        if (name_start) {
            name_start += 6;
            const char *name_end = strchr(name_start, '"');
            if (name_end) {
                size_t name_len = name_end - name_start;
                char name[128];
                if (name_len > 127) name_len = 127;
                memcpy(name, name_start, name_len);
                name[name_len] = 0;

                /* Parse args attribute */
                cJSON *args_obj = NULL;
                const char *args_start = strstr(name_end, "args=\"");
                if (args_start) {
                    args_start += 6;
                    /* Find closing "/> — need to handle escaped quotes */
                    const char *args_end = strstr(args_start, "\"/>");
                    if (!args_end) args_end = strstr(args_start, "\" />");
                    if (args_end) {
                        size_t alen = args_end - args_start;
                        char *args_str = (char *)malloc(alen + 1);
                        memcpy(args_str, args_start, alen);
                        args_str[alen] = 0;
                        /* Unescape HTML entities */
                        args_obj = cJSON_Parse(args_str);
                        free(args_str);
                    }
                }

                cJSON *result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "tool", name);
                cJSON_AddItemToObject(result, "arguments",
                    args_obj ? args_obj : cJSON_CreateObject());
                return result;
            }
        }
    }

    /* Strategy 2: Fallback to existing JSON extraction */
    char *json_str = llm_extract_json(response);
    if (json_str) {
        cJSON *parsed = cJSON_Parse(json_str);
        free(json_str);
        if (parsed && cJSON_GetObjectItem(parsed, "tool")) {
            return parsed;
        }
        if (parsed) cJSON_Delete(parsed);
    }

    return NULL;
}
