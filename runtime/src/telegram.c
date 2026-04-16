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

/* Callback query queue (ring buffer) */
#define TG_CB_QUEUE_SIZE 16
static struct {
    long long chat_id;
    long long msg_id;
    long long query_id;
    char      data[256];
} g_cb_queue[TG_CB_QUEUE_SIZE];
static int g_cb_head = 0;
static int g_cb_tail = 0;
static int g_cb_count = 0;

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

static void enqueue_callback(long long chat_id, long long msg_id,
                              long long query_id, const char *data) {
    if (g_cb_count >= TG_CB_QUEUE_SIZE) {
        g_cb_head = (g_cb_head + 1) % TG_CB_QUEUE_SIZE;
        g_cb_count--;
    }
    g_cb_queue[g_cb_tail].chat_id = chat_id;
    g_cb_queue[g_cb_tail].msg_id = msg_id;
    g_cb_queue[g_cb_tail].query_id = query_id;
    strncpy(g_cb_queue[g_cb_tail].data, data, sizeof(g_cb_queue[g_cb_tail].data) - 1);
    g_cb_queue[g_cb_tail].data[sizeof(g_cb_queue[g_cb_tail].data) - 1] = 0;
    g_cb_tail = (g_cb_tail + 1) % TG_CB_QUEUE_SIZE;
    g_cb_count++;
}

/* Escape text for embedding in JSON string */
static size_t json_escape(const char *src, char *dst, size_t dst_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 2; i++) {
        if (src[i] == '"' || src[i] == '\\') dst[j++] = '\\';
        if (src[i] == '\n') { dst[j++] = '\\'; dst[j++] = 'n'; continue; }
        if (src[i] == '\r') continue;
        dst[j++] = src[i];
    }
    dst[j] = 0;
    return j;
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

        cJSON *chat_id_item = cJSON_GetObjectItem(msg, "chat_id");
        if (!cJSON_IsNumber(chat_id_item)) { cJSON_Delete(root); return; }

        long long cid = (long long)chat_id_item->valuedouble;

        cJSON *content = cJSON_GetObjectItem(msg, "content");
        if (!content) { cJSON_Delete(root); return; }

        cJSON *content_type = cJSON_GetObjectItem(content, "@type");
        if (!cJSON_IsString(content_type)) { cJSON_Delete(root); return; }

        /* Text messages */
        if (strcmp(content_type->valuestring, "messageText") == 0) {
            cJSON *text_obj = cJSON_GetObjectItem(content, "text");
            if (!text_obj) { cJSON_Delete(root); return; }
            cJSON *text_str = cJSON_GetObjectItem(text_obj, "text");
            if (!cJSON_IsString(text_str)) {
                if (cJSON_IsString(text_obj)) text_str = text_obj;
                else { cJSON_Delete(root); return; }
            }
            if (text_str->valuestring[0]) {
                enqueue_message(cid, text_str->valuestring);
                fprintf(stderr, "[TG] Message from %lld: %s\n", cid, text_str->valuestring);
            }
        }
        /* Document messages — download file to workspace */
        else if (strcmp(content_type->valuestring, "messageDocument") == 0) {
            cJSON *doc = cJSON_GetObjectItem(content, "document");
            if (doc) {
                cJSON *doc_obj = cJSON_GetObjectItem(doc, "document");
                if (doc_obj) {
                    cJSON *doc_id = cJSON_GetObjectItem(doc_obj, "id");
                    if (cJSON_IsNumber(doc_id)) {
                        char req[256];
                        snprintf(req, sizeof(req),
                            "{\"@type\":\"downloadFile\",\"file_id\":%lld,\"priority\":1}",
                            (long long)doc_id->valuedouble);
                        my_send(req);
                        fprintf(stderr, "[TG] Downloading document file_id=%lld\n",
                                (long long)doc_id->valuedouble);
                    }
                }
                cJSON *fname = cJSON_GetObjectItem(doc, "file_name");
                if (cJSON_IsString(fname)) {
                    /* Enqueue a notification that a file was received */
                    char notify[512];
                    snprintf(notify, sizeof(notify), "[FILE] Received: %s", fname->valuestring);
                    enqueue_message(cid, notify);
                }
            }
        }
    }
    /* Callback query (inline keyboard button press) */
    else if (strcmp(type->valuestring, "updateNewCallbackQuery") == 0) {
        cJSON *chat_id_item = cJSON_GetObjectItem(root, "chat_id");
        cJSON *msg_id_item = cJSON_GetObjectItem(root, "message_id");
        cJSON *query_id_item = cJSON_GetObjectItem(root, "id");
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        const char *cb_data = "";
        if (payload) {
            cJSON *data_item = cJSON_GetObjectItem(payload, "data");
            if (cJSON_IsString(data_item)) cb_data = data_item->valuestring;
        }

        long long cid = cJSON_IsNumber(chat_id_item) ? (long long)chat_id_item->valuedouble : 0;
        long long mid = cJSON_IsNumber(msg_id_item) ? (long long)msg_id_item->valuedouble : 0;
        long long qid = cJSON_IsNumber(query_id_item) ? (long long)query_id_item->valuedouble : 0;

        enqueue_callback(cid, mid, qid, cb_data);
        fprintf(stderr, "[TG] Callback query from %lld: %s\n", cid, cb_data);
    }
    /* Message reaction */
    else if (strcmp(type->valuestring, "updateMessageReaction") == 0) {
        /* Future: enqueue reaction events */
        fprintf(stderr, "[TG] Message reaction received\n");
    }

    cJSON_Delete(root);
}

/* ---- Public API ---- */

void tg_init(const char *bot_token) {
    if (g_client) return;

    strncpy(g_bot_token, bot_token, sizeof(g_bot_token) - 1);
    g_bot_token[sizeof(g_bot_token) - 1] = 0;

    g_queue_head = g_queue_tail = g_queue_count = 0;
    g_cb_head = g_cb_tail = g_cb_count = 0;
    g_ready = 0;

    g_client = td_json_client_create();
    if (!g_client) {
        fprintf(stderr, "[TG] Failed to create TDLib client\n");
        return;
    }

    fprintf(stderr, "[TG] Authenticating bot...\n");

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

    int count_before = g_queue_count + g_cb_count;
    handle_update(resp);
    return ((g_queue_count + g_cb_count) > count_before) ? 1 : 0;
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

int tg_get_next_callback(long long *chat_id, long long *msg_id,
                          long long *query_id, char *data, size_t data_size) {
    if (g_cb_count == 0) return 0;

    *chat_id = g_cb_queue[g_cb_head].chat_id;
    *msg_id = g_cb_queue[g_cb_head].msg_id;
    *query_id = g_cb_queue[g_cb_head].query_id;
    strncpy(data, g_cb_queue[g_cb_head].data, data_size - 1);
    data[data_size - 1] = 0;

    g_cb_head = (g_cb_head + 1) % TG_CB_QUEUE_SIZE;
    g_cb_count--;
    return 1;
}

/* Escape text for JSON embedding */
static void escape_json_text(const char *text, char *escaped, size_t esc_size) {
    size_t j = 0;
    for (size_t i = 0; text[i] && j < esc_size - 2; i++) {
        if (text[i] == '"' || text[i] == '\\') escaped[j++] = '\\';
        if (text[i] == '\n') { escaped[j++] = '\\'; escaped[j++] = 'n'; continue; }
        if (text[i] == '\r') continue;
        escaped[j++] = text[i];
    }
    escaped[j] = 0;
}

void tg_send_message(long long chat_id, const char *text) {
    if (!g_client || !g_ready) return;

    char escaped[4096];
    escape_json_text(text, escaped, sizeof(escaped));

    char req[8192];
    snprintf(req, sizeof(req),
        "{\"@type\":\"sendMessage\","
        "\"@extra\":\"tg_send\","
        "\"chat_id\":%lld,"
        "\"input_message_content\":{\"@type\":\"inputMessageText\","
        "\"text\":{\"@type\":\"formattedText\",\"text\":\"%s\"}}}",
        chat_id, escaped);

    my_send(req);

    for (int i = 0; i < 20; i++) {
        const char *resp = my_receive(1.0);
        if (!resp) break;
        if (strstr(resp, "\"tg_send\"")) {
            fprintf(stderr, "[TG] sendMessage confirmed\n");
            break;
        }
    }
}

long long tg_send_message_ex(long long chat_id, const char *text) {
    if (!g_client || !g_ready) return 0;

    char escaped[4096];
    escape_json_text(text, escaped, sizeof(escaped));

    char req[8192];
    snprintf(req, sizeof(req),
        "{\"@type\":\"sendMessage\","
        "\"@extra\":\"tg_send_ex\","
        "\"chat_id\":%lld,"
        "\"input_message_content\":{\"@type\":\"inputMessageText\","
        "\"text\":{\"@type\":\"formattedText\",\"text\":\"%s\"}}}",
        chat_id, escaped);

    my_send(req);

    /* Drain until we get the message object with its ID */
    for (int i = 0; i < 30; i++) {
        const char *resp = my_receive(1.0);
        if (!resp) break;
        if (strstr(resp, "\"tg_send_ex\"")) {
            /* Parse the message ID from the response */
            cJSON *r = cJSON_Parse(resp);
            if (r) {
                cJSON *msg = cJSON_GetObjectItem(r, "message");
                if (msg) {
                    cJSON *mid = cJSON_GetObjectItem(msg, "id");
                    if (cJSON_IsNumber(mid)) {
                        long long id = (long long)mid->valuedouble;
                        cJSON_Delete(r);
                        fprintf(stderr, "[TG] sendMessage_ex returned id=%lld\n", id);
                        return id;
                    }
                }
                cJSON_Delete(r);
            }
            break;
        }
    }
    return 0;
}

void tg_send_message_inline(long long chat_id, const char *text, const char *buttons_json) {
    if (!g_client || !g_ready) return;

    char escaped[4096];
    escape_json_text(text, escaped, sizeof(escaped));

    /* Build inline keyboard markup from buttons_json */
    /* buttons_json: [[{"text":"Label","callback_data":"value"}]] */
    char req[16384];
    snprintf(req, sizeof(req),
        "{\"@type\":\"sendMessage\","
        "\"chat_id\":%lld,"
        "\"input_message_content\":{\"@type\":\"inputMessageText\","
        "\"text\":{\"@type\":\"formattedText\",\"text\":\"%s\"}},"
        "\"reply_markup\":{\"@type\":\"replyInlineMarkup\","
        "\"rows\":%s}}",
        chat_id, escaped, buttons_json);

    my_send(req);
    fprintf(stderr, "[TG] Sent message with inline keyboard to %lld\n", chat_id);

    /* Drain response */
    for (int i = 0; i < 10; i++) {
        const char *resp = my_receive(0.5);
        if (!resp) break;
        if (strstr(resp, "sendMessage")) break;
    }
}

void tg_edit_message(long long chat_id, long long msg_id, const char *new_text) {
    if (!g_client || !g_ready) return;

    char escaped[4096];
    escape_json_text(new_text, escaped, sizeof(escaped));

    char req[8192];
    snprintf(req, sizeof(req),
        "{\"@type\":\"editMessageText\","
        "\"chat_id\":%lld,"
        "\"message_id\":%lld,"
        "\"input_message_content\":{\"@type\":\"inputMessageText\","
        "\"text\":{\"@type\":\"formattedText\",\"text\":\"%s\"}}}",
        chat_id, msg_id, escaped);

    my_send(req);

    /* Drain response */
    for (int i = 0; i < 10; i++) {
        const char *resp = my_receive(0.5);
        if (!resp) break;
        if (strstr(resp, "editMessageText")) break;
    }
}

void tg_answer_callback_query(long long query_id, const char *text) {
    if (!g_client || !g_ready) return;

    char escaped[512];
    escape_json_text(text ? text : "", escaped, sizeof(escaped));

    char req[1024];
    snprintf(req, sizeof(req),
        "{\"@type\":\"answerCallbackQuery\","
        "\"callback_query_id\":%lld,"
        "\"text\":\"%s\"}",
        query_id, escaped);

    /* answerCallbackQuery is synchronous via execute */
    const char *resp = my_execute(req);
    (void)resp;
    fprintf(stderr, "[TG] Answered callback query %lld\n", query_id);
}

void tg_set_reaction(long long chat_id, long long msg_id, const char *emoji) {
    if (!g_client || !g_ready) return;

    char req[512];
    snprintf(req, sizeof(req),
        "{\"@type\":\"setMessageReaction\","
        "\"chat_id\":%lld,"
        "\"message_id\":%lld,"
        "\"reaction_type\":{\"@type\":\"reactionTypeEmoji\","
        "\"emoji\":\"%s\"}}",
        chat_id, msg_id, emoji ? emoji : "");

    my_send(req);
}

void tg_send_file(long long chat_id, const char *path, const char *caption) {
    if (!g_client || !g_ready) return;

    char esc_caption[1024];
    escape_json_text(caption ? caption : "", esc_caption, sizeof(esc_caption));

    char req[2048];
    snprintf(req, sizeof(req),
        "{\"@type\":\"sendMessage\","
        "\"chat_id\":%lld,"
        "\"input_message_content\":{\"@type\":\"inputMessageDocument\","
        "\"document\":{\"@type\":\"inputFileLocal\",\"path\":\"%s\"},"
        "\"caption\":{\"@type\":\"formattedText\",\"text\":\"%s\"}}}",
        chat_id, path, esc_caption);

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
void tg_send_message_inline(long long chat_id, const char *text, const char *buttons) {
    (void)chat_id; (void)text; (void)buttons;
}
long long tg_send_message_ex(long long chat_id, const char *text) {
    (void)chat_id; (void)text; return 0;
}
void tg_edit_message(long long chat_id, long long msg_id, const char *new_text) {
    (void)chat_id; (void)msg_id; (void)new_text;
}
void tg_answer_callback_query(long long query_id, const char *text) {
    (void)query_id; (void)text;
}
void tg_set_reaction(long long chat_id, long long msg_id, const char *emoji) {
    (void)chat_id; (void)msg_id; (void)emoji;
}
int tg_get_next_callback(long long *chat_id, long long *msg_id,
                          long long *query_id, char *data, size_t data_size) {
    (void)chat_id; (void)msg_id; (void)query_id; (void)data; (void)data_size;
    return 0;
}

#endif
