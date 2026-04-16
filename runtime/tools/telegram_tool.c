#define _CRT_SECURE_NO_WARNINGS
#define TOOL_BUILDING_DLL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tool_api.h"
#include "tool_helpers.h"

#ifdef ENABLE_TELEGRAM

#include "telegram.h"

static cJSON *telegram_send(cJSON *args, const char *workspace, char **error) {
    (void)workspace;

    cJSON *chat_id_item = cJSON_GetObjectItem(args, "chat_id");
    cJSON *text_item    = cJSON_GetObjectItem(args, "text");

    if (!cJSON_IsNumber(chat_id_item)) TOOL_ERROR("telegram_send requires 'chat_id' (number)");
    if (!cJSON_IsString(text_item))    TOOL_ERROR("telegram_send requires 'text' (string)");

    if (!tg_is_ready()) TOOL_ERROR("Telegram not initialized");

    tg_send_message(chat_id_item->valuedouble, text_item->valuestring);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "sent");
    cJSON_AddNumberToObject(r, "chat_id", chat_id_item->valuedouble);
    return r;
}

static cJSON *telegram_send_file(cJSON *args, const char *workspace, char **error) {
    (void)workspace;

    cJSON *chat_id_item = cJSON_GetObjectItem(args, "chat_id");
    cJSON *path_item    = cJSON_GetObjectItem(args, "path");
    cJSON *caption_item = cJSON_GetObjectItem(args, "caption");

    if (!cJSON_IsNumber(chat_id_item)) TOOL_ERROR("telegram_send_file requires 'chat_id' (number)");
    if (!cJSON_IsString(path_item))    TOOL_ERROR("telegram_send_file requires 'path' (string)");

    if (!tg_is_ready()) TOOL_ERROR("Telegram not initialized");

    tg_send_file(chat_id_item->valuedouble, path_item->valuestring,
                 cJSON_IsString(caption_item) ? caption_item->valuestring : NULL);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "sent");
    cJSON_AddStringToObject(r, "path", path_item->valuestring);
    return r;
}

static ToolInfo info = {
    TOOL_API_VERSION_MAX,
    sizeof(ToolInfo),
    "telegram_send",
    "Send a text message via Telegram bot",
    0,  /* requires_workspace */
    1,  /* is_idempotent */
    0   /* has_side_effects */
};

TOOL_EXPORT const ToolInfo *tool_get_info(void) { return &info; }
TOOL_EXPORT tool_execute_fn tool_get_execute(void) { return telegram_send; }

#else /* !ENABLE_TELEGRAM */

/* Stub tool when Telegram is not compiled in */
static cJSON *telegram_stub(cJSON *args, const char *workspace, char **error) {
    (void)args; (void)workspace;
    *error = _strdup("Telegram not available (compile with ENABLE_TELEGRAM)");
    return NULL;
}

static ToolInfo info = {
    TOOL_API_VERSION_MAX,
    sizeof(ToolInfo),
    "telegram_send",
    "Send a text message via Telegram bot (not compiled)",
    0, 0, 0
};

TOOL_EXPORT const ToolInfo *tool_get_info(void) { return &info; }
TOOL_EXPORT tool_execute_fn tool_get_execute(void) { return telegram_stub; }

#endif
