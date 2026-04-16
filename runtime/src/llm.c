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
