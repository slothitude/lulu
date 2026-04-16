#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "telegram.h"
#include "cJSON.h"

#ifdef ENABLE_TELEGRAM

#include <td/telegram/td_json_client.h>
#include <td/telegram/td_log.h>

/* ---- Internal state ---- */
static void  *g_client = NULL;
static int    g_ready  = 0;
static char   g_bot_token[256] = {0};
static int    g_last_update_id = 0;

/* Incoming message queue (ring buffer) */
#define TG_QUEUE_SIZE 64
static struct {
    long long chat_id;
    char      text[1024];
} g_queue[TG_QUEUE_SIZE];
static int g_queue_head = 0;
static int g_queue_tail = 0;
static int g_queue_count = 0;

/* ---- Helpers ---- */

static void enqueue_message(long long chat_id, const char *text) {
    if (g_queue_count >= TG_QUEUE_SIZE) {
        /* drop oldest */
        g_queue_head = (g_queue_head + 1) % TG_QUEUE_SIZE;
        g_queue_count--;
    }
    g_queue[g_queue_tail].chat_id = chat_id;
    strncpy(g_queue[g_queue_tail].text, text, sizeof(g_queue[g_queue_tail].text) - 1);
    g_queue[g_queue_tail].text[sizeof(g_queue[g_queue_tail].text) - 1] = 0;
    g_queue_tail = (g_queue_tail + 1) % TG_QUEUE_SIZE;
    g_queue_count++;
}

static void my_send(const char *json) {
    if (!g_client) return;
    td_json_client_send(g_client, json);
}

static const char *my_receive(double timeout) {
    if (!g_client) return NULL;
    return td_json_client_receive(g_client, timeout);
}

static const char *my_execute(const char *json) {
    if (!g_client) return NULL;
    return td_json_client_execute(g_client, json);
}

/* ---- Auth flow ---- */

static void handle_auth_state(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    cJSON *auth = cJSON_GetObjectItem(root, "authorization_state");
    if (!auth) { cJSON_Delete(root); return; }

    cJSON *type = cJSON_GetObjectItem(auth, "@type");
    if (!cJSON_IsString(type)) { cJSON_Delete(root); return; }

    const char *t = type->valuestring;
    fprintf(stderr, "[TG] Auth state: %s\n", t);

    if (strcmp(t, "authorizationStateWaitTdlibParameters") == 0) {
        my_send(
            "{\"@type\":\"setTdlibParameters\","
            "\"database_directory\":\"tg_data\","
            "\"use_message_database\":false,"
            "\"use_secret_chats\":false,"
            "\"use_file_database\":false,"
            "\"use_chat_info_database\":false,"
            "\"api_id\":94575,\"api_hash\":\"a3406de8d171bb422bb6ddf3bbd800e2\","
            "\"system_language_code\":\"en\","
            "\"device_model\":\"BashAgent\","
            "\"application_version\":\"2.0\","
            "\"enable_storage_optimizer\":true}");
        fprintf(stderr, "[TG] Sent tdlib parameters\n");
    }
    else if (strcmp(t, "authorizationStateWaitPhoneNumber") == 0) {
        char req[512];
        snprintf(req, sizeof(req),
            "{\"@type\":\"checkAuthenticationBotToken\","
            "\"token\":\"%s\"}", g_bot_token);
        my_send(req);
        fprintf(stderr, "[TG] Sent bot token for auth\n");
    }
    else if (strcmp(t, "authorizationStateReady") == 0) {
        g_ready = 1;
        fprintf(stderr, "[TG] Auth complete, bot ready\n");
    }
    else if (strcmp(t, "authorizationStateClosed") == 0) {
        fprintf(stderr, "[TG] Auth closed unexpectedly\n");
    }
    else if (strcmp(t, "authorizationStateClosing") == 0) {
        fprintf(stderr, "[TG] Client closing...\n");
    }

    cJSON_Delete(root);
}

/* ---- Message parsing ---- */

static void handle_update(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    cJSON *type = cJSON_GetObjectItem(root, "@type");
    if (!cJSON_IsString(type)) { cJSON_Delete(root); return; }

    if (strcmp(type->valuestring, "updateAuthorizationState") == 0) {
        handle_auth_state(json);
    }
    else if (strcmp(type->valuestring, "error") == 0) {
        cJSON *code = cJSON_GetObjectItem(root, "code");
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        fprintf(stderr, "[TG] TDLib error: code=%d msg=%s\n",
                cJSON_IsNumber(code) ? code->valueint : 0,
                cJSON_IsString(msg) ? msg->valuestring : "?");
    }
    else if (strcmp(type->valuestring, "updateNewMessage") == 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        if (!msg) { cJSON_Delete(root); return; }

        /* Only handle regular text messages from users */
        cJSON *chat_id_item = cJSON_GetObjectItem(msg, "chat_id");
        if (!cJSON_IsNumber(chat_id_item)) { cJSON_Delete(root); return; }

        long long cid = (long long)chat_id_item->valuedouble;

        /* Extract text from content */
        cJSON *content = cJSON_GetObjectItem(msg, "content");
        if (!content) { cJSON_Delete(root); return; }

        cJSON *content_type = cJSON_GetObjectItem(content, "@type");
        if (!cJSON_IsString(content_type) ||
            strcmp(content_type->valuestring, "messageText") != 0) {
            cJSON_Delete(root);
            return;
        }

        cJSON *text_obj = cJSON_GetObjectItem(content, "text");
        if (!text_obj) { cJSON_Delete(root); return; }

        /* text can be a formattedText object with "text" field, or a string */
        cJSON *text_str = cJSON_GetObjectItem(text_obj, "text");
        if (!cJSON_IsString(text_str)) {
            /* Maybe text_obj itself is a string */
            if (cJSON_IsString(text_obj)) text_str = text_obj;
            else { cJSON_Delete(root); return; }
        }

        if (text_str->valuestring[0]) {
            enqueue_message(cid, text_str->valuestring);
            fprintf(stderr, "[TG] Message from %lld: %s\n", cid, text_str->valuestring);
        }
    }

    cJSON_Delete(root);
}

/* ---- Public API ---- */

void tg_init(const char *bot_token) {
    if (g_client) return;

    strncpy(g_bot_token, bot_token, sizeof(g_bot_token) - 1);
    g_bot_token[sizeof(g_bot_token) - 1] = 0;

    g_queue_head = g_queue_tail = g_queue_count = 0;
    g_ready = 0;

    g_client = td_json_client_create();
    if (!g_client) {
        fprintf(stderr, "[TG] Failed to create TDLib client\n");
        return;
    }

    fprintf(stderr, "[TG] Authenticating bot...\n");

    /* Wait for auth (with timeout ~120s) — key generation can take time */
    for (int i = 0; i < 240 && !g_ready; i++) {
        const char *resp = my_receive(0.5);
        if (resp) {
            handle_update(resp);
        }
    }

    if (!g_ready) {
        fprintf(stderr, "[TG] Auth timed out\n");
    }
}

void tg_shutdown(void) {
    if (!g_client) return;

    my_send("{\"@type\":\"close\"}");

    /* Drain remaining events */
    for (int i = 0; i < 20; i++) {
        const char *resp = my_receive(0.5);
        if (resp) {
            if (strstr(resp, "authorizationStateClosed")) {
                break;
            }
        }
    }

    td_json_client_destroy(g_client);
    g_client = NULL;
    g_ready = 0;
    fprintf(stderr, "[TG] Shutdown complete\n");
}

int tg_poll(double timeout) {
    if (!g_client) return 0;

    const char *resp = my_receive(timeout);
    if (!resp) return 0;

    int count_before = g_queue_count;
    handle_update(resp);
    return (g_queue_count > count_before) ? 1 : 0;
}

int tg_get_next_message(long long *chat_id, char *text, size_t text_size) {
    if (g_queue_count == 0) return 0;

    *chat_id = g_queue[g_queue_head].chat_id;
    strncpy(text, g_queue[g_queue_head].text, text_size - 1);
    text[text_size - 1] = 0;

    g_queue_head = (g_queue_head + 1) % TG_QUEUE_SIZE;
    g_queue_count--;
    return 1;
}

void tg_send_message(long long chat_id, const char *text) {
    if (!g_client || !g_ready) return;

    /* Escape text for JSON */
    char escaped[4096];
    size_t j = 0;
    for (size_t i = 0; text[i] && j < sizeof(escaped) - 2; i++) {
        if (text[i] == '"' || text[i] == '\\') escaped[j++] = '\\';
        if (text[i] == '\n') { escaped[j++] = '\\'; escaped[j++] = 'n'; continue; }
        escaped[j++] = text[i];
    }
    escaped[j] = 0;

    char req[8192];
    snprintf(req, sizeof(req),
        "{\"@type\":\"sendMessage\","
        "\"@extra\":\"tg_send\","
        "\"chat_id\":%lld,"
        "\"input_message_content\":{\"@type\":\"inputMessageText\","
        "\"text\":{\"@type\":\"formattedText\",\"text\":\"%s\"}}}",
        chat_id, escaped);

    /* sendMessage is async in TDLib — send then drain until we get the matching response */
    my_send(req);

    /* Drain responses until we find our sendMessage confirmation or timeout */
    for (int i = 0; i < 20; i++) {
        const char *resp = my_receive(1.0);
        if (!resp) break;
        /* Check if this is our response (matched by @extra field) */
        if (strstr(resp, "\"tg_send\"")) {
            fprintf(stderr, "[TG] sendMessage confirmed\n");
            break;
        }
        /* It's some other update — process it but keep draining */
    }
}

void tg_send_file(long long chat_id, const char *path, const char *caption) {
    if (!g_client || !g_ready) return;

    char req[2048];
    snprintf(req, sizeof(req),
        "{\"@type\":\"sendMessage\","
        "\"chat_id\":%lld,"
        "\"input_message_content\":{\"@type\":\"inputMessageDocument\","
        "\"document\":{\"@type\":\"inputFileLocal\",\"path\":\"%s\"},"
        "\"caption\":{\"@type\":\"formattedText\",\"text\":\"%s\"}}}",
        chat_id, path, caption ? caption : "");

    my_send(req);
    const char *resp = my_receive(2.0);
    (void)resp;
}

int tg_is_ready(void) {
    return g_ready;
}

#else /* !ENABLE_TELEGRAM */

/* Stubs when Telegram is not compiled in */
void tg_init(const char *bot_token) { (void)bot_token; }
void tg_shutdown(void) {}
int  tg_poll(double timeout) { (void)timeout; return 0; }
int  tg_get_next_message(long long *chat_id, char *text, size_t text_size) {
    (void)chat_id; (void)text; (void)text_size; return 0;
}
void tg_send_message(long long chat_id, const char *text) {
    (void)chat_id; (void)text;
}
void tg_send_file(long long chat_id, const char *path, const char *caption) {
    (void)chat_id; (void)path; (void)caption;
}
int  tg_is_ready(void) { return 0; }

#endif
