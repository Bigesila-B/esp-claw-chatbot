/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_core_internal.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "claw_utils_string.h"
#include "claw_event_publisher.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "claw_core";

#define CLAW_CORE_STAGE_REASONING_SNIPPET_LEN 150

#if CONFIG_CLAW_CORE_STAGE_VERBOSITY_VERBOSE
static atomic_bool s_tool_progress_enabled = true;
#else
static atomic_bool s_tool_progress_enabled;
#endif

void claw_core_set_tool_progress_enabled(bool enabled)
{
    atomic_store(&s_tool_progress_enabled, enabled);
}

const char *claw_core_chat_error(const char *detail)
{
    if (!detail) detail = "";
    if (strstr(detail, "storage is full") || strstr(detail, "ENOSPC")) {
        return "设备存储已满。请在网页「文件管理」释放空间，再重新发送问题。";
    }
    if (strncmp(detail, "HTTP ", 5) == 0) {
        int status = atoi(detail + 5);
        if (status == 429) return "聊天模型 API 已限流。请稍后再试。生图等工具任务请先确认任务状态，再决定是否重发。";
        if (status == 401 || status == 403 || status == 402) return "模型 API 拒绝了请求。请检查密钥和账户额度。";
        if (status == 404) return "找不到配置的模型或 API 地址。请检查模型设置。";
        if (status >= 500) return "模型服务暂时不可用。请稍后发送 /retry。";
    }
    if (strstr(detail, "TIMEOUT") || strstr(detail, "timed out")) {
        return "等待模型回复超时。请稍后发送 /retry。";
    }
    if (strstr(detail, "ESP_ERR_HTTP_CONNECT") || strstr(detail, "ESP_ERR_HTTP_FETCH_HEADER")) {
        return "连接模型服务失败。请检查网络，再发送 /retry。";
    }
    if (strstr(detail, "NO_MEM") || strstr(detail, "Out of memory")) {
        return "设备可用内存不足。请稍后发送 /retry。";
    }
    if (strcmp(detail, "Consecutive MCP tool failures") == 0) {
        return "MCP 工具连续调用失败。生图时可用 /mcp models 核对服务支持的模型 ID，再重发原请求。";
    }
    if (strcmp(detail, "request cancelled") == 0) return "本次回复已取消。";
    if (strcmp(detail, "Text retry cannot execute tools") == 0) {
        return "这条问题需要调用工具。请查看原任务状态，或重新发送一条指令。";
    }
    return "本次聊天未完成。请发送 /retry 重试上一条文字问题。";
}

const char *claw_core_failure_note_reason(const char *text)
{
    static const char prefix[] = "Session note: the previous request failed before producing a final answer.\nReason: ";
    return text && strncmp(text, prefix, sizeof(prefix) - 1) == 0 ? text + sizeof(prefix) - 1 : NULL;
}

static esp_err_t build_response_payload_json(const claw_core_request_t *request,
                                             const claw_core_response_t *response,
                                             const char *error_text,
                                             bool persisted,
                                             const char *image_url,
                                             char **out_payload_json)
{
    cJSON *root = NULL;
    char *payload_json = NULL;

    if (!request || !response || !out_payload_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_payload_json = NULL;

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    if (!cJSON_AddNumberToObject(root, "request_id", (double)request->request_id) ||
            !cJSON_AddStringToObject(root,
                                     "status",
                                     response->status == CLAW_CORE_RESPONSE_STATUS_OK ? "ok" : "error")) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if ((request->session_id && !cJSON_AddStringToObject(root, "session_id", request->session_id)) ||
            (persisted && !cJSON_AddBoolToObject(root, "persisted", true)) ||
            (error_text && !cJSON_AddStringToObject(root, "error_message", error_text))) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (image_url && !error_text) (void)cJSON_AddStringToObject(root, "reply_image_url", image_url);

    payload_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload_json) {
        return ESP_ERR_NO_MEM;
    }

    *out_payload_json = payload_json;
    return ESP_OK;
}

static esp_err_t build_out_message_event_common(const char *event_id_prefix,
                                                const char *event_type,
                                                uint32_t request_id,
                                                int64_t now_ms,
                                                const char *channel,
                                                const char *chat_id,
                                                const char *text,
                                                claw_event_t *out_event)
{
    if (!event_id_prefix || !event_id_prefix[0] || !event_type || !event_type[0] ||
            !channel || !channel[0] || !chat_id || !chat_id[0] ||
            !text || !text[0] || !out_event) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_event, 0, sizeof(*out_event));
    snprintf(out_event->event_id, sizeof(out_event->event_id),
             "%s-%" PRIu32 "-%" PRId64, event_id_prefix, request_id, now_ms);
    strlcpy(out_event->source_cap, "claw_core", sizeof(out_event->source_cap));
    strlcpy(out_event->event_type, event_type, sizeof(out_event->event_type));
    strlcpy(out_event->source_channel, channel, sizeof(out_event->source_channel));
    strlcpy(out_event->chat_id, chat_id, sizeof(out_event->chat_id));
    strlcpy(out_event->content_type, "text", sizeof(out_event->content_type));
    out_event->text = (char *)text;
    out_event->timestamp_ms = now_ms;
    out_event->session_policy = CLAW_SESSION_POLICY_CHAT;
    return ESP_OK;
}

static esp_err_t build_agent_out_message_event(const claw_core_request_t *request,
                                               const claw_core_response_t *response,
                                               bool persisted,
                                               const char *image_url,
                                               const char *display_text,
                                               claw_event_t *out_event,
                                               char **out_payload_json)
{
    const char *channel = NULL;
    const char *chat_id = NULL;
    const char *text = NULL;
    int64_t now_ms;
    esp_err_t err;

    if (!request || !response || !out_event || !out_payload_json) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_payload_json = NULL;

    const char *error_text = response->status == CLAW_CORE_RESPONSE_STATUS_ERROR ?
                             claw_core_chat_error(response->error_message) : NULL;
    text = error_text ? error_text : (display_text ? display_text : response->text);
    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    err = build_response_payload_json(request, response, error_text, persisted, image_url, out_payload_json);
    if (err != ESP_OK) {
        return err;
    }

    now_ms = claw_utils_time_now_ms();
    channel = (response->target_channel && response->target_channel[0]) ?
              response->target_channel : request->source_channel;
    chat_id = (response->target_chat_id && response->target_chat_id[0]) ?
              response->target_chat_id : request->source_chat_id;

    err = build_out_message_event_common("agent",
                                         "out_message",
                                         request->request_id,
                                         now_ms,
                                         channel,
                                         chat_id,
                                         text,
                                         out_event);
    if (err != ESP_OK) {
        free(*out_payload_json);
        *out_payload_json = NULL;
        return err;
    }

    snprintf(out_event->message_id, sizeof(out_event->message_id),
             "agent-%" PRIu32,
             request->request_id);
    if (request->source_message_id && request->source_message_id[0]) {
        strlcpy(out_event->correlation_id,
                request->source_message_id,
                sizeof(out_event->correlation_id));
        if (request->source_channel && strcmp(request->source_channel, "qq") == 0 &&
                strcmp(channel, request->source_channel) == 0 && request->source_chat_id &&
                strcmp(chat_id, request->source_chat_id) == 0) {
            out_event->reply_to_message_id = (char *)request->source_message_id;
        }
    } else {
        snprintf(out_event->correlation_id, sizeof(out_event->correlation_id),
                 "%" PRIu32,
                 request->request_id);
    }
    out_event->payload_json = *out_payload_json;

    return ESP_OK;
}

void claw_core_publish_out_message_if_requested(const claw_core_request_item_t *request,
                                                const claw_core_response_item_t *response,
                                                bool persisted, const char *image_url, const char *display_text,
                                                bool (*deliver)(const struct claw_event *event))
{
    claw_event_t event = {0};
    char *payload_json = NULL;
    esp_err_t err;

    if (!request || !response ||
            !(request->view.flags & CLAW_CORE_REQUEST_FLAG_PUBLISH_OUT_MESSAGE)) {
        return;
    }

    err = build_agent_out_message_event(&request->view,
                                        &response->view,
                                        persisted,
                                        image_url,
                                        display_text,
                                        &event,
                                        &payload_json);
    if (err == ESP_OK && !(deliver && !display_text && response->view.status == CLAW_CORE_RESPONSE_STATUS_OK && deliver(&event))) {
        err = claw_event_router_publish(&event);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to publish out_message for request_id=%" PRIu32 ": %s",
                 request->view.request_id,
                 esp_err_to_name(err));
    }

    free(payload_json);
}

esp_err_t claw_core_publish_stage_text(const claw_core_request_t *request, const char *text)
{
    claw_event_t event = {0};
    int64_t now_ms;
    esp_err_t err;

    if (!request || !text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!(request->flags & CLAW_CORE_REQUEST_FLAG_PUBLISH_STAGE_MESSAGE)) {
        return ESP_OK;
    }

    now_ms = claw_utils_time_now_ms();
    err = build_out_message_event_common(
        "stage",
        "agent_stage",
        request->request_id,
        now_ms,
        (request->target_channel && request->target_channel[0]) ?
            request->target_channel : request->source_channel,
        (request->target_chat_id && request->target_chat_id[0]) ?
            request->target_chat_id : request->source_chat_id,
        text,
        &event);
    if (err != ESP_OK) {
        return err;
    }

    esp_err_t pub_err = claw_event_router_publish(&event);
    if (pub_err != ESP_OK) {
        ESP_LOGW(TAG, "request=%" PRIu32 " failed to publish stage event: %s",
                 request->request_id, esp_err_to_name(pub_err));
    }
    return pub_err;
}

void claw_core_publish_stage_tool_calls(const claw_core_request_t *request,
                                        const claw_core_llm_response_t *response,
                                        uint32_t iteration)
{
    char buf[256];
    size_t off = 0;
    size_t i;
    int written;

    if (!response || !atomic_load(&s_tool_progress_enabled)) {
        return;
    }

    if (response->reasoning_content && response->reasoning_content[0]) {
        size_t reasoning_len = strlen(response->reasoning_content);
        size_t prefix_len = claw_utils_utf8_prefix_len(response->reasoning_content,
                                                 CLAW_CORE_STAGE_REASONING_SNIPPET_LEN);

        written = snprintf(buf, sizeof(buf), "🦞 [Round %" PRIu32 "] %.*s%s",
                           iteration + 1,
                           (int)prefix_len,
                           response->reasoning_content,
                           reasoning_len > prefix_len ? "..." : "");
        if (written < 0 || (size_t)written >= sizeof(buf)) {
            return;
        }
        (void)claw_core_publish_stage_text(request, buf);
        return;
    }

    if (response->tool_call_count == 0) {
        return;
    }

    written = snprintf(buf, sizeof(buf), "🦞 [Round %" PRIu32 "] Snap: ", iteration + 1);
    if (written < 0 || (size_t)written >= sizeof(buf)) {
        return;
    }
    off = (size_t)written;

    for (i = 0; i < response->tool_call_count; i++) {
        const char *name = response->tool_calls[i].name ? response->tool_calls[i].name : "?";
        const char *args = response->tool_calls[i].arguments_json;
        if (args && args[0]) {
            size_t arg_len = strlen(args);
            size_t prefix_len = claw_utils_utf8_prefix_len(args, 40);
            written = snprintf(buf + off, sizeof(buf) - off, "%s%s(%.*s%s)",
                               i == 0 ? "" : ", ", name, (int)prefix_len, args,
                               arg_len > prefix_len ? "..." : "");
        } else {
            written = snprintf(buf + off, sizeof(buf) - off, "%s%s",
                               i == 0 ? "" : ", ", name);
        }
        if (written < 0 || (size_t)written >= sizeof(buf) - off) {
            break;
        }
        off += (size_t)written;
    }

    (void)claw_core_publish_stage_text(request, buf);
}

void claw_core_publish_stage_note_for_round(claw_core_state_t *core,
                                            const claw_core_request_t *request,
                                            uint32_t round_index)
{
    char *stage_note = NULL;
    esp_err_t err;

    if (!core || !core->collect_stage_note) {
        return;
    }

    err = core->collect_stage_note(request, &stage_note, core->collect_stage_note_user_ctx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stage_note callback failed: %s", esp_err_to_name(err));
        free(stage_note);
        return;
    }

    char buf[256];
    int written;

    if (!stage_note || !stage_note[0] || !atomic_load(&s_tool_progress_enabled)) {
        free(stage_note);
        return;
    }

    written = snprintf(buf, sizeof(buf), "🦞 [Round %" PRIu32 "] %s", round_index + 1, stage_note);
    if (written < 0 || (size_t)written >= sizeof(buf)) {
        free(stage_note);
        return;
    }
    (void)claw_core_publish_stage_text(request, buf);
    free(stage_note);
}
