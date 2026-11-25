/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"
#include "app_cloud_media.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "claw_session_mgr.h"
#include "claw_agent_mgr.h"
#include "claw_memory.h"
#include "claw_event_router.h"
#include "claw_event_publisher.h"
#include "esp_log.h"

#define SESSION_URI "/api/webim/sessions"
#define WEBIM_MESSAGE_MAX 8192

esp_err_t http_server_webim_error(httpd_req_t *req, esp_err_t err)
{
    const char *status = "500 Internal Server Error", *code = "storage_error";
    const char *message = "设备无法读取或保存会话。请检查存储后重试。";
    switch (err) {
    case ESP_ERR_NOT_FOUND:
        status = "404 Not Found"; code = "session_not_found"; message = "找不到这个会话。"; break;
    case ESP_ERR_INVALID_ARG:
        status = "400 Bad Request"; code = "invalid_request"; message = "消息格式有误。请检查文字和附件。"; break;
    case ESP_ERR_INVALID_SIZE:
        status = "413 Content Too Large"; code = "content_too_large"; message = "消息太长，请缩短内容后再发。"; break;
    case ESP_ERR_INVALID_STATE:
        status = "409 Conflict"; code = "session_busy"; message = "会话状态已有变化，请刷新后重试。"; break;
    case ESP_ERR_NOT_SUPPORTED:
        status = "409 Conflict"; code = "channel_unavailable"; message = "此聊天通道暂时无法发送消息。"; break;
    case ESP_ERR_NOT_ALLOWED:
        status = "409 Conflict"; code = "stt_unavailable"; message = "请先在 LLM 设置中启用云端语音转写模型。"; break;
    case ESP_ERR_NO_MEM:
    case ESP_ERR_TIMEOUT:
        status = "503 Service Unavailable"; code = "busy_or_full"; message = "设备较忙，请稍后重试。"; break;
    case CLAW_CORE_ERR_STORAGE_FULL:
        status = "507 Insufficient Storage"; code = "storage_full"; message = claw_core_chat_error("storage is full"); break;
    default:
        break;
    }
    cJSON *body = cJSON_CreateObject();
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    cJSON_AddStringToObject(body, "error", code);
    cJSON_AddStringToObject(body, "message", message);
    httpd_resp_set_status(req, status);
    ESP_LOGW("http_sessions", "%s: %s", req->uri, esp_err_to_name(err));
    return http_server_send_json_response(req, body);
}

static const char *reply_channel(const claw_session_info_t *info)
{
    return strcmp(info->channel, "local") == 0 ? "web" : info->channel;
}

static bool can_send(const claw_session_info_t *info)
{
    return info->agent_id == 0 && claw_event_router_channel_is_bound(reply_channel(info));
}

static cJSON *session_json(const claw_session_info_t *info)
{
    cJSON *item = cJSON_CreateObject();
    if (!item) return NULL;
    cJSON_AddStringToObject(item, "session", info->id);
    cJSON_AddStringToObject(item, "title", info->title);
    cJSON_AddStringToObject(item, "source", info->channel);
    cJSON_AddStringToObject(item, "chat_id", info->chat_id);
    cJSON_AddStringToObject(item, "alias", info->alias);
    cJSON_AddNumberToObject(item, "activity_order", info->activity);
    cJSON_AddStringToObject(item, "delivery_state", info->delivery_state);
    cJSON_AddStringToObject(item, "reply_channel", reply_channel(info));
    cJSON_AddBoolToObject(item, "can_send", can_send(info));
    cJSON_AddStringToObject(item, "run_state", claw_agent_mgr_session_state(info->session_id));
    cJSON_AddStringToObject(item, "boot_id", http_server_ctx()->boot_id);
    return item;
}

static esp_err_t query_number(httpd_req_t *req, const char *key, size_t fallback, size_t max, size_t *out)
{
    char buf[24];
    *out = fallback;
    if (http_server_query_get(req, key, buf, sizeof(buf)) != ESP_OK) return ESP_OK;
    if (!buf[0]) return ESP_ERR_INVALID_ARG;
    for (const char *p = buf; *p; p++) if (!isdigit((unsigned char)*p)) return ESP_ERR_INVALID_ARG;
    char *end;
    unsigned long value = strtoul(buf, &end, 10);
    if (*end || value > max) return ESP_ERR_INVALID_ARG;
    *out = value;
    return ESP_OK;
}

static esp_err_t sessions_handler(httpd_req_t *req)
{
    claw_session_info_t *info = calloc(1, sizeof(*info));
    cJSON *result = NULL;
    if (!info) return http_server_webim_error(req, ESP_ERR_NO_MEM);
    esp_err_t err = ESP_OK;
    if (req->method == HTTP_POST) {
        err = claw_session_mgr_catalog_create(info);
        if (err == ESP_OK) { result = session_json(info); httpd_resp_set_status(req, "201 Created"); }
    } else {
        size_t offset = 0, limit = 30;
        char source[16] = "all";
        http_server_query_get(req, "source", source, sizeof(source));
        err = query_number(req, "cursor", 0, 100000, &offset);
        if (err == ESP_OK) err = query_number(req, "limit", 30, 50, &limit);
        if (err == ESP_OK) err = claw_session_mgr_catalog_list(source, offset, limit, &result);
        cJSON *items = cJSON_GetObjectItem(result, "items");
        for (cJSON *item = items ? items->child : NULL; item;) {
            cJSON *next = item->next;
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "session"));
            err = claw_session_mgr_catalog_get(id, info);
            if (err != ESP_OK) break;
            if (!info->title[0]) {
                cJSON *history = NULL;
                esp_err_t read_err = claw_memory_read_session_messages(info->session_id, 0, 30, &history);
                if (read_err == ESP_OK && cJSON_GetNumberValue(cJSON_GetObjectItem(history, "total")) == 0) {
                    cJSON_Delete(history);
                    cJSON_Delete(cJSON_DetachItemViaPointer(items, item));
                    item = next;
                    continue;
                }
                cJSON *message;
                cJSON_ArrayForEach(message, cJSON_GetObjectItem(history, "messages")) {
                    const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(message, "role"));
                    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(message, "text"));
                    if (!role || strcmp(role, "user") != 0 || !text || !text[0]) continue;
                    size_t n = strnlen(text, sizeof(info->title) - 1);
                    while (n && ((unsigned char)text[n] & 0xc0) == 0x80) n--;
                    memcpy(info->title, text, n); info->title[n] = 0;
                    if (claw_session_mgr_catalog_rename(id, info->title) != ESP_OK) ESP_LOGW("http_sessions", "Cannot save history title");
                    cJSON_ReplaceItemInObject(item, "title", cJSON_CreateString(info->title));
                    break;
                }
                cJSON_Delete(history);
            }
            cJSON_AddStringToObject(item, "reply_channel", reply_channel(info));
            cJSON_AddBoolToObject(item, "can_send", can_send(info));
            cJSON_AddStringToObject(item, "run_state", claw_agent_mgr_session_state(info->session_id));
            item = next;
        }
        if (result) cJSON_AddStringToObject(result, "boot_id", http_server_ctx()->boot_id);
    }
    free(info);
    if (err != ESP_OK || !result) { cJSON_Delete(result); return http_server_webim_error(req, err != ESP_OK ? err : ESP_ERR_NO_MEM); }
    return http_server_send_json_response(req, result);
}

static esp_err_t session_handler(httpd_req_t *req)
{
    const char *path = req->uri + strlen(SESSION_URI) + 1;
    size_t length = strcspn(path, "/?");
    if (length != CLAW_SESSION_PUBLIC_ID_SIZE - 1) return http_server_webim_error(req, ESP_ERR_INVALID_ARG);
    char id[CLAW_SESSION_PUBLIC_ID_SIZE];
    memcpy(id, path, length); id[length] = 0;
    for (size_t i = 0; i < length; i++) if (!isxdigit((unsigned char)id[i])) return http_server_webim_error(req, ESP_ERR_INVALID_ARG);
    const char *suffix = path + length;
    bool messages = strncmp(suffix, "/messages", 9) == 0 && (suffix[9] == 0 || suffix[9] == '?');
    if (!messages && *suffix && *suffix != '?') return http_server_webim_error(req, ESP_ERR_NOT_FOUND);
    claw_session_info_t *info = calloc(1, sizeof(*info));
    if (!info) return http_server_webim_error(req, ESP_ERR_NO_MEM);
    esp_err_t err = claw_session_mgr_catalog_get(id, info);
    cJSON *result = NULL, *body = NULL;
    if (err == ESP_OK && messages && req->method == HTTP_GET) {
        size_t before = 0, limit = 30;
        err = query_number(req, "before", 0, 1000000, &before);
        if (err == ESP_OK) err = query_number(req, "limit", 30, 50, &limit);
        if (err == ESP_OK) err = claw_memory_read_session_messages(info->session_id, before, limit, &result);
        if (err == ESP_OK) {
            cJSON_AddStringToObject(result, "delivery_state", info->delivery_state);
            cJSON_AddStringToObject(result, "run_state", claw_agent_mgr_session_state(info->session_id));
            cJSON_AddStringToObject(result, "boot_id", http_server_ctx()->boot_id);
        }
    } else if (err == ESP_OK && !messages) {
        if (req->method == HTTP_PATCH) {
            err = http_server_parse_json_body(req, &body);
            const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(body, "title"));
            if (err == ESP_OK) err = claw_session_mgr_catalog_rename(id, title);
            if (err == ESP_OK) { strlcpy(info->title, title, sizeof(info->title)); result = session_json(info); }
        } else if (req->method == HTTP_DELETE) {
            err = claw_agent_mgr_delete_session(id);
            if (err == ESP_OK) { result = cJSON_CreateObject(); cJSON_AddBoolToObject(result, "ok", true); }
        } else result = session_json(info);
    } else if (err == ESP_OK) err = ESP_ERR_INVALID_ARG;
    free(info); cJSON_Delete(body);
    if (err != ESP_OK || !result) { cJSON_Delete(result); return http_server_webim_error(req, err != ESP_OK ? err : ESP_ERR_NO_MEM); }
    return http_server_send_json_response(req, result);
}

static bool has_command_prefix(const char *text, const char *prefix)
{
    size_t size = strlen(prefix);
    return strncmp(text, prefix, size) == 0 &&
           (!text[size] || isspace((unsigned char)text[size]));
}

static esp_err_t post_web_message(const claw_session_info_t *info, const char *text,
                                  const char *message_id, uint32_t *run_id)
{
    while (isspace((unsigned char)*text)) text++;
    if (*text == '/' || app_cloud_media_is_setup_message(text)) {
        claw_event_t *event = calloc(1, sizeof(*event));
        if (!event) return ESP_ERR_NO_MEM;
        strlcpy(event->event_id, message_id, sizeof(event->event_id));
        strlcpy(event->source_cap, "web_chat", sizeof(event->source_cap));
        strlcpy(event->source_channel, "web", sizeof(event->source_channel));
        strlcpy(event->chat_id, info->id, sizeof(event->chat_id));
        strlcpy(event->sender_id, "web_user", sizeof(event->sender_id));
        strlcpy(event->event_type, "message", sizeof(event->event_type));
        strlcpy(event->content_type, "text", sizeof(event->content_type));
        strlcpy(event->message_id, message_id, sizeof(event->message_id));
        event->session_policy = CLAW_SESSION_POLICY_CHAT;
        event->text = (char *)text;
        esp_err_t err = ESP_OK;
        bool handled = claw_event_router_try_chat_command(event, &err);
        if (!handled && (has_command_prefix(text, "/session") || has_command_prefix(text, "/llm") ||
                         has_command_prefix(text, "/mcp models"))) {
            strlcpy(event->chat_id, strcmp(info->channel, "web") == 0 ? info->chat_id : "web-ui",
                    sizeof(event->chat_id));
            strlcpy(event->target_endpoint, info->id, sizeof(event->target_endpoint));
            err = claw_event_router_publish(event);
            handled = true;
        }
        free(event);
        if (handled) return err;
    }
    return claw_agent_mgr_post_session_message(info->id, text, message_id,
                                               strcmp(reply_channel(info), "web") != 0, run_id);
}

esp_err_t http_server_webim_send_session(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = http_server_parse_json_body(req, &body);
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(body, "session"));
    const char *message_id = cJSON_GetStringValue(cJSON_GetObjectItem(body, "client_message_id"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(body, "text"));
    bool media_setup = app_cloud_media_is_setup_message(text);
    const char *legacy_chat = cJSON_GetStringValue(cJSON_GetObjectItem(body, "chat_id"));
    const bool legacy = !id && !message_id && legacy_chat && legacy_chat[0] && strlen(legacy_chat) < 96;
    cJSON *files = cJSON_GetObjectItem(body, "files");
    if (err == ESP_OK && media_setup && cJSON_GetArraySize(files)) err = ESP_ERR_INVALID_ARG;
    const char *audio = NULL;
    char *combined = calloc(1, WEBIM_MESSAGE_MAX + 1);
    claw_session_info_t *info = calloc(1, sizeof(*info));
    if (!combined || !info) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK && !legacy && (!id || strlen(id) != 32 || !message_id || !message_id[0] || strlen(message_id) > 64 || !text)) err = ESP_ERR_INVALID_ARG;
    if (err == ESP_OK && text && strlen(text) > WEBIM_MESSAGE_MAX) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK && text) strlcpy(combined, text, WEBIM_MESSAGE_MAX + 1);
    if (err == ESP_OK && files && (!cJSON_IsArray(files) || cJSON_GetArraySize(files) > 4)) err = ESP_ERR_INVALID_ARG;
    cJSON *file;
    cJSON_ArrayForEach(file, files) {
        if (err != ESP_OK) break;
        const char *path = cJSON_GetStringValue(file);
        if (!path || strncmp(path, "/inbox/webim/", 13) != 0 || !http_server_path_is_safe(path)) { err = ESP_ERR_INVALID_ARG; break; }
        if (app_cloud_media_is_audio_file(path)) {
            if (legacy || cJSON_GetArraySize(files) != 1) { err = ESP_ERR_INVALID_ARG; break; }
            audio = path;
        }
        size_t used = strlen(combined);
        int n = snprintf(combined + used, WEBIM_MESSAGE_MAX + 1 - used, "\n/files%s", path);
        if (n < 0 || n >= WEBIM_MESSAGE_MAX + 1 - used) err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && !combined[0]) err = ESP_ERR_INVALID_ARG;
    if (err == ESP_OK && legacy) {
        err = claw_event_router_publish_message("local_gateway", "web", legacy_chat, combined, "web_user", NULL);
        cJSON_Delete(body); free(combined); free(info);
        if (err != ESP_OK) return http_server_webim_error(req, err);
        cJSON *result = cJSON_CreateObject();
        if (!result) return http_server_webim_error(req, ESP_ERR_NO_MEM);
        cJSON_AddBoolToObject(result, "ok", true);
        return http_server_send_json_response(req, result);
    }
    if (err == ESP_OK) err = claw_session_mgr_catalog_get(id, info);
    if (err == ESP_OK && !can_send(info)) err = ESP_ERR_NOT_SUPPORTED;
    http_server_ctx_t *ctx = http_server_ctx();
    uint32_t run_id = 0;
    bool duplicate = false;
    const char *receipt_text = media_setup ? "/tts setup" : combined;
    if (err == ESP_OK) {
        for (size_t i = 0; i < HTTP_WEBIM_RECEIPTS; i++) {
            http_webim_receipt_t *old = &ctx->webim_receipts[i];
            if (strcmp(old->message_id, message_id) != 0) continue;
            duplicate = true;
            if (strcmp(old->session, id) != 0 || !old->text || strcmp(old->text, receipt_text) != 0) err = ESP_ERR_INVALID_STATE;
            else run_id = old->run_id;
            break;
        }
    }
    if (err == ESP_OK && !duplicate) {
        if (audio) {
            char *path = malloc(HTTP_SERVER_PATH_MAX);
            err = path ? http_server_resolve_storage_path(audio, path, HTTP_SERVER_PATH_MAX) : ESP_ERR_NO_MEM;
            if (err == ESP_OK) err = app_cloud_media_web_transcribe(info->id, message_id, path, text,
                                                                   strcmp(reply_channel(info), "web") != 0);
            if (err == ESP_ERR_NOT_FOUND) err = ESP_ERR_NOT_ALLOWED;
            free(path);
        } else {
            err = post_web_message(info, combined, message_id, &run_id);
        }
        if (err == ESP_OK) {
            http_webim_receipt_t *slot = &ctx->webim_receipts[ctx->webim_receipt_next++ % HTTP_WEBIM_RECEIPTS];
            free(slot->text);
            slot->text = media_setup ? strdup(receipt_text) : combined;
            if (!media_setup) combined = NULL;
            strlcpy(slot->message_id, message_id, sizeof(slot->message_id));
            strlcpy(slot->session, id, sizeof(slot->session));
            slot->run_id = run_id;
        }
    }
    cJSON *result = NULL;
    if (err == ESP_OK && (result = cJSON_CreateObject())) {
        cJSON_AddNumberToObject(result, "run_id", run_id);
        cJSON_AddStringToObject(result, "client_message_id", message_id);
        cJSON_AddStringToObject(result, "state", "queued");
        cJSON_AddStringToObject(result, "boot_id", ctx->boot_id);
        httpd_resp_set_status(req, "202 Accepted");
    }
    cJSON_Delete(body); free(combined); free(info);
    if (err != ESP_OK || !result) return http_server_webim_error(req, err != ESP_OK ? err : ESP_ERR_NO_MEM);
    return http_server_send_json_response(req, result);
}

esp_err_t http_server_register_webim_session_routes(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = SESSION_URI, .method = HTTP_GET, .handler = sessions_handler},
        {.uri = SESSION_URI, .method = HTTP_POST, .handler = sessions_handler},
        {.uri = SESSION_URI "/*", .method = HTTP_GET, .handler = session_handler},
        {.uri = SESSION_URI "/*", .method = HTTP_PATCH, .handler = session_handler},
        {.uri = SESSION_URI "/*", .method = HTTP_DELETE, .handler = session_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
