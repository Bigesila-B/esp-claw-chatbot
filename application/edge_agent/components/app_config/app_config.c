/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings_store.h"

#define APP_DEFAULT_STICKER_RATE "50"

#if CONFIG_APP_CLAW_CAP_MCP_CLIENT
#include "cap_mcp_client.h"
#include "esp_log.h"

static esp_err_t app_config_load_mcp_servers(void);
#endif

typedef struct {
    const char *key;
    const char *default_value;
    size_t offset;
    size_t size;
} app_config_field_t;

typedef struct {
    const char *legacy_id;
    const char *backend_type;
    const char *base_url;
    const char *auth_type;
    const char *max_tokens_field;
    const char *default_image_max_bytes;
    const char *supports_tools;
    const char *supports_vision;
    const char *image_remote_url_only;
} app_config_legacy_llm_preset_t;

#define APP_CONFIG_FIELD(member, nvs_key, default_literal) \
    { nvs_key, default_literal, offsetof(app_config_t, member), sizeof(((app_config_t *)0)->member) }

#define APP_DEFAULT_LLM_API_KEY              ""
#define APP_DEFAULT_LLM_BACKEND_TYPE         ""
#define APP_DEFAULT_LLM_MODEL                ""
#define APP_DEFAULT_LLM_BASE_URL             ""
#define APP_DEFAULT_LLM_AUTH_TYPE            ""
#define APP_DEFAULT_LLM_TIMEOUT_MS           "120000"
#define APP_DEFAULT_LLM_MAX_TOKENS           "8192"
/* 0 表示按模型名自动匹配上下文窗口大小 */
#define APP_DEFAULT_LLM_CONTEXT_WINDOW       "0"
#define APP_DEFAULT_LLM_DEFAULT_IMAGE_MAX_BYTES "524288"
#define APP_DEFAULT_LLM_MAX_TOKENS_FIELD     ""
#define APP_DEFAULT_LLM_SUPPORTS_TOOLS       "false"
#define APP_DEFAULT_LLM_SUPPORTS_VISION      "false"
#define APP_DEFAULT_LLM_IMAGE_REMOTE_URL_ONLY "false"
#define APP_DEFAULT_QQ_APP_ID                ""
#define APP_DEFAULT_QQ_APP_SECRET            ""
#define APP_DEFAULT_QQ_MSG_TYPE              "0"
#define APP_DEFAULT_FEISHU_APP_ID            ""
#define APP_DEFAULT_FEISHU_APP_SECRET        ""
#define APP_DEFAULT_TG_BOT_TOKEN             ""
#define APP_DEFAULT_WECHAT_TOKEN             ""
#define APP_DEFAULT_WECHAT_BASE_URL          "https://ilinkai.weixin.qq.com"
#define APP_DEFAULT_WECHAT_CDN_BASE_URL      "https://novac2c.cdn.weixin.qq.com/c2c"
#define APP_DEFAULT_WECHAT_ACCOUNT_ID        "default"
#define APP_DEFAULT_SEARCH_BRAVE_KEY         ""
#define APP_DEFAULT_SEARCH_TAVILY_KEY        ""
#define APP_DEFAULT_ENABLED_CAP_GROUPS       ""
#define APP_DEFAULT_LLM_VISIBLE_CAP_GROUPS   ""
#define APP_DEFAULT_ENABLED_LUA_MODULES      ""
#define APP_DEFAULT_TIME_TIMEZONE            "CST-8"

static const app_config_field_t s_fields[] = {
    APP_CONFIG_FIELD(wifi_ssid, "wifi_ssid", APP_WIFI_SSID),
    APP_CONFIG_FIELD(wifi_password, "wifi_password", APP_WIFI_PASSWORD),
    APP_CONFIG_FIELD(ap_ssid, "ap_ssid", ""),
    APP_CONFIG_FIELD(ap_password, "ap_password", ""),
    APP_CONFIG_FIELD(ap_behavior, "ap_behavior", "keep"),
    APP_CONFIG_FIELD(show_tool_progress, "show_tool_prog", "false"),
    APP_CONFIG_FIELD(auto_stickers, "auto_stickers", "true"),
    APP_CONFIG_FIELD(sticker_rate, "sticker_rate", APP_DEFAULT_STICKER_RATE),
    APP_CONFIG_FIELD(tts_reply_mode, "tts_reply_mode", "0"),
    APP_CONFIG_FIELD(llm_api_key, "llm_api_key", APP_DEFAULT_LLM_API_KEY),
    APP_CONFIG_FIELD(llm_backend_type, "llm_backend", APP_DEFAULT_LLM_BACKEND_TYPE),
    APP_CONFIG_FIELD(llm_model, "llm_model", APP_DEFAULT_LLM_MODEL),
    APP_CONFIG_FIELD(llm_base_url, "llm_base_url", APP_DEFAULT_LLM_BASE_URL),
    APP_CONFIG_FIELD(llm_auth_type, "llm_auth_type", APP_DEFAULT_LLM_AUTH_TYPE),
    APP_CONFIG_FIELD(llm_timeout_ms, "llm_timeout_ms", APP_DEFAULT_LLM_TIMEOUT_MS),
    APP_CONFIG_FIELD(llm_max_tokens, "llm_max_tokens", APP_DEFAULT_LLM_MAX_TOKENS),
    APP_CONFIG_FIELD(llm_context_window, "llm_ctx_window", APP_DEFAULT_LLM_CONTEXT_WINDOW),
    APP_CONFIG_FIELD(llm_default_image_max_bytes, "llm_img_max_b", APP_DEFAULT_LLM_DEFAULT_IMAGE_MAX_BYTES),
    APP_CONFIG_FIELD(llm_max_tokens_field, "llm_max_toks_f", APP_DEFAULT_LLM_MAX_TOKENS_FIELD),
    APP_CONFIG_FIELD(llm_supports_tools, "llm_sup_tools", APP_DEFAULT_LLM_SUPPORTS_TOOLS),
    APP_CONFIG_FIELD(llm_supports_vision, "llm_sup_vis", APP_DEFAULT_LLM_SUPPORTS_VISION),
    APP_CONFIG_FIELD(llm_image_remote_url_only, "llm_img_url_o", APP_DEFAULT_LLM_IMAGE_REMOTE_URL_ONLY),
    APP_CONFIG_FIELD(qq_app_id, "qq_app_id", APP_DEFAULT_QQ_APP_ID),
    APP_CONFIG_FIELD(qq_app_secret, "qq_app_secret", APP_DEFAULT_QQ_APP_SECRET),
    APP_CONFIG_FIELD(qq_msg_type, "qq_msg_type", APP_DEFAULT_QQ_MSG_TYPE),
    APP_CONFIG_FIELD(feishu_app_id, "feishu_app_id", APP_DEFAULT_FEISHU_APP_ID),
    APP_CONFIG_FIELD(feishu_app_secret, "feishu_secret", APP_DEFAULT_FEISHU_APP_SECRET),
    APP_CONFIG_FIELD(tg_bot_token, "tg_bot_token", APP_DEFAULT_TG_BOT_TOKEN),
    APP_CONFIG_FIELD(wechat_token, "wechat_token", APP_DEFAULT_WECHAT_TOKEN),
    APP_CONFIG_FIELD(wechat_base_url, "wechat_base_url", APP_DEFAULT_WECHAT_BASE_URL),
    APP_CONFIG_FIELD(wechat_cdn_base_url, "wechat_cdn_url", APP_DEFAULT_WECHAT_CDN_BASE_URL),
    APP_CONFIG_FIELD(wechat_account_id, "wechat_acct_id", APP_DEFAULT_WECHAT_ACCOUNT_ID),
    APP_CONFIG_FIELD(search_brave_key, "brave_key", APP_DEFAULT_SEARCH_BRAVE_KEY),
    APP_CONFIG_FIELD(search_tavily_key, "tavily_key", APP_DEFAULT_SEARCH_TAVILY_KEY),
    APP_CONFIG_FIELD(search_http_allowlist, "http_allow_ls", APP_SEARCH_HTTP_ALLOWLIST),
#if CONFIG_APP_CLAW_CAP_MCP_CLIENT
    APP_CONFIG_FIELD(mcp_server_url, "mcp_server_url", ""),
#endif
    APP_CONFIG_FIELD(enabled_cap_groups, "en_cap_groups", APP_DEFAULT_ENABLED_CAP_GROUPS),
    APP_CONFIG_FIELD(llm_visible_cap_groups, "vis_cap_groups", APP_DEFAULT_LLM_VISIBLE_CAP_GROUPS),
    APP_CONFIG_FIELD(enabled_lua_modules, "en_lua_mods", APP_DEFAULT_ENABLED_LUA_MODULES),
    APP_CONFIG_FIELD(time_timezone, "time_timezone", APP_DEFAULT_TIME_TIMEZONE),
};

// for backward compatibility, migrate from old settings to new settings
static const app_config_legacy_llm_preset_t s_legacy_llm_presets[] = {
    {
        .legacy_id = "openai",
        .backend_type = "openai_compatible",
        .base_url = "https://api.openai.com/v1",
        .auth_type = "bearer",
        .max_tokens_field = "max_completion_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "true",
        .image_remote_url_only = "false",
    },
    {
        .legacy_id = "qwen",
        .backend_type = "openai_compatible",
        .base_url = "https://dashscope.aliyuncs.com/compatible-mode/v1",
        .auth_type = "bearer",
        .max_tokens_field = "max_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "true",
        .image_remote_url_only = "false",
    },
    {
        .legacy_id = "qwen_compatible",
        .backend_type = "openai_compatible",
        .base_url = "https://dashscope.aliyuncs.com/compatible-mode/v1",
        .auth_type = "bearer",
        .max_tokens_field = "max_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "true",
        .image_remote_url_only = "false",
    },
    {
        .legacy_id = "deepseek",
        .backend_type = "openai_compatible",
        .base_url = "https://api.deepseek.com",
        .auth_type = "bearer",
        .max_tokens_field = "max_completion_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "true",
        .image_remote_url_only = "false",
    },
    {
        .legacy_id = "custom_openai_compatible",
        .backend_type = "openai_compatible",
        .base_url = "https://api.openai.com/v1",
        .auth_type = "bearer",
        .max_tokens_field = "max_completion_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "true",
        .image_remote_url_only = "false",
    },
    {
        .legacy_id = "anthropic",
        .backend_type = "anthropic_compatible",
        .base_url = "https://api.anthropic.com/v1",
        .auth_type = "none",
        .max_tokens_field = "max_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "true",
        .image_remote_url_only = "false",
    },
    {
        .legacy_id = "claude",
        .backend_type = "anthropic_compatible",
        .base_url = "https://api.anthropic.com/v1",
        .auth_type = "none",
        .max_tokens_field = "max_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "true",
        .image_remote_url_only = "false",
    },
};

static inline char *app_config_field_ptr(app_config_t *config, const app_config_field_t *field)
{
    return (char *)config + field->offset;
}

static inline const char *app_config_field_cptr(const app_config_t *config, const app_config_field_t *field)
{
    return (const char *)config + field->offset;
}

static bool app_config_ap_behavior_is_valid(const char *ap_behavior)
{
    return !ap_behavior || ap_behavior[0] == '\0' ||
           strcmp(ap_behavior, "keep") == 0 ||
           strcmp(ap_behavior, "close_on_sta") == 0;
}

static const app_config_legacy_llm_preset_t *app_config_find_legacy_llm_preset(const char *legacy_id)
{
    size_t i;

    if (!legacy_id || !legacy_id[0]) {
        return NULL;
    }

    for (i = 0; i < sizeof(s_legacy_llm_presets) / sizeof(s_legacy_llm_presets[0]); i++) {
        if (strcmp(s_legacy_llm_presets[i].legacy_id, legacy_id) == 0) {
            return &s_legacy_llm_presets[i];
        }
    }

    return NULL;
}

static esp_err_t app_config_write_if_empty(const char *key,
                                           char *current_value,
                                           size_t current_value_size,
                                           const char *fallback_value)
{
    if (!key || !current_value || !fallback_value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (current_value[0] != '\0') {
        return ESP_OK;
    }

    esp_err_t err = settings_store_set_string(key, fallback_value);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(current_value, fallback_value, current_value_size);
    return ESP_OK;
}

static esp_err_t app_config_replace_if_match(const char *key,
                                             char *current_value,
                                             size_t current_value_size,
                                             const char *match_value,
                                             const char *replacement_value)
{
    esp_err_t err;

    if (!key || !current_value || !match_value || !replacement_value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(current_value, match_value) != 0) {
        return ESP_OK;
    }

    err = settings_store_set_string(key, replacement_value);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(current_value, replacement_value, current_value_size);
    return ESP_OK;
}

static esp_err_t app_config_upgrade_legacy_llm_settings(void)
{
    char legacy_profile[32] = {0};
    char legacy_provider[32] = {0};
    char llm_backend_type[32] = {0};
    char llm_base_url[APP_CONFIG_STR_LEN] = {0};
    char llm_auth_type[32] = {0};
    char llm_max_tokens_field[32] = {0};
    char llm_default_image_max_bytes[16] = {0};
    char llm_supports_tools[8] = {0};
    char llm_supports_vision[8] = {0};
    char llm_image_remote_url_only[8] = {0};
    const app_config_legacy_llm_preset_t *preset = NULL;
    const char *legacy_id = NULL;
    esp_err_t err;

    err = settings_store_get_string("llm_profile", legacy_profile, sizeof(legacy_profile), "");
    if (err != ESP_OK) {
        return err;
    }
    err = settings_store_get_string("llm_provider", legacy_provider, sizeof(legacy_provider), "");
    if (err != ESP_OK) {
        return err;
    }

    legacy_id = legacy_profile[0] ? legacy_profile : legacy_provider;
    preset = app_config_find_legacy_llm_preset(legacy_id);
    if (preset) {
        err = settings_store_get_string("llm_backend", llm_backend_type, sizeof(llm_backend_type), "");
        if (err != ESP_OK) {
            return err;
        }
        err = settings_store_get_string("llm_base_url", llm_base_url, sizeof(llm_base_url), "");
        if (err != ESP_OK) {
            return err;
        }
        err = settings_store_get_string("llm_auth_type", llm_auth_type, sizeof(llm_auth_type), "");
        if (err != ESP_OK) {
            return err;
        }
        err = settings_store_get_string("llm_max_toks_f", llm_max_tokens_field, sizeof(llm_max_tokens_field), "");
        if (err != ESP_OK) {
            return err;
        }
        err = settings_store_get_string("llm_img_max_b",
                                        llm_default_image_max_bytes,
                                        sizeof(llm_default_image_max_bytes),
                                        "");
        if (err != ESP_OK) {
            return err;
        }
        err = settings_store_get_string("llm_sup_tools", llm_supports_tools, sizeof(llm_supports_tools), "");
        if (err != ESP_OK) {
            return err;
        }
        err = settings_store_get_string("llm_sup_vis", llm_supports_vision, sizeof(llm_supports_vision), "");
        if (err != ESP_OK) {
            return err;
        }
        err = settings_store_get_string("llm_img_url_o",
                                        llm_image_remote_url_only,
                                        sizeof(llm_image_remote_url_only),
                                        "");
        if (err != ESP_OK) {
            return err;
        }

        err = app_config_write_if_empty("llm_backend",
                                        llm_backend_type,
                                        sizeof(llm_backend_type),
                                        preset->backend_type);
        if (err != ESP_OK) {
            return err;
        }
        err = app_config_write_if_empty("llm_base_url",
                                        llm_base_url,
                                        sizeof(llm_base_url),
                                        preset->base_url);
        if (err != ESP_OK) {
            return err;
        }
        err = app_config_write_if_empty("llm_auth_type",
                                        llm_auth_type,
                                        sizeof(llm_auth_type),
                                        preset->auth_type);
        if (err != ESP_OK) {
            return err;
        }
        err = app_config_write_if_empty("llm_max_toks_f",
                                        llm_max_tokens_field,
                                        sizeof(llm_max_tokens_field),
                                        preset->max_tokens_field);
        if (err != ESP_OK) {
            return err;
        }
        err = app_config_write_if_empty("llm_img_max_b",
                                        llm_default_image_max_bytes,
                                        sizeof(llm_default_image_max_bytes),
                                        preset->default_image_max_bytes);
        if (err != ESP_OK) {
            return err;
        }
        err = app_config_write_if_empty("llm_sup_tools",
                                        llm_supports_tools,
                                        sizeof(llm_supports_tools),
                                        preset->supports_tools);
        if (err != ESP_OK) {
            return err;
        }
        err = app_config_write_if_empty("llm_sup_vis",
                                        llm_supports_vision,
                                        sizeof(llm_supports_vision),
                                        preset->supports_vision);
        if (err != ESP_OK) {
            return err;
        }
        err = app_config_write_if_empty("llm_img_url_o",
                                        llm_image_remote_url_only,
                                        sizeof(llm_image_remote_url_only),
                                        preset->image_remote_url_only);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = settings_store_get_string("llm_backend", llm_backend_type, sizeof(llm_backend_type), "");
    if (err != ESP_OK) {
        return err;
    }
    err = app_config_replace_if_match("llm_backend",
                                      llm_backend_type,
                                      sizeof(llm_backend_type),
                                      "anthropic",
                                      "anthropic_compatible");
    if (err != ESP_OK) {
        return err;
    }

    err = settings_store_erase_key("llm_profile");
    if (err != ESP_OK) {
        return err;
    }
    err = settings_store_erase_key("llm_provider");
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

esp_err_t app_config_init(void)
{
    esp_err_t err = settings_store_init(&(settings_store_config_t) {
        .namespace_name = "app",
    });
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_APP_CLAW_CAP_MCP_CLIENT
    err = app_config_load_mcp_servers();
    if (err != ESP_OK) {
        ESP_LOGW("app_config", "Cannot load MCP servers: %s", esp_err_to_name(err));
    }
#endif
    return app_config_upgrade_legacy_llm_settings();
}

void app_config_load_defaults(app_config_t *config)
{
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));

    for (size_t i = 0; i < sizeof(s_fields) / sizeof(s_fields[0]); ++i) {
        strlcpy(app_config_field_ptr(config, &s_fields[i]),
                s_fields[i].default_value ? s_fields[i].default_value : "",
                s_fields[i].size);
    }
}

#if CONFIG_APP_CLAW_CAP_MCP_CLIENT
/* Store the token and its URL in one NVS key. An interrupted save that updates
 * only mcp_server_url must not send the old token to the new server. */
#define APP_MCP_BEARER_KEY "mcp_bearer"
#define APP_MCP_BEARER_RECORD_SIZE (2 * APP_CONFIG_STR_LEN)
#define APP_MCP_SERVERS_KEY "mcp_servers_v1"
#define APP_MCP_SERVERS_RECORD_SIZE 4096

static cJSON *mcp_servers_json(const cap_mcp_servers_t *servers, bool include_tokens)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(root, "servers");
    bool ok = items != NULL;
    for (size_t i = 0; ok && i < servers->count; ++i) {
        const cap_mcp_server_config_t *entry = &servers->items[i];
        cJSON *row = cJSON_CreateObject();
        if (!cJSON_AddItemToArray(items, row)) {
            cJSON_Delete(row);
            ok = false;
            break;
        }
        ok = cJSON_AddNumberToObject(row, "id", entry->id) &&
             cJSON_AddStringToObject(row, "name", entry->name) &&
             cJSON_AddStringToObject(row, "server_url", entry->url) &&
             (include_tokens ? cJSON_AddStringToObject(row, "bearer_token", entry->token) :
                               cJSON_AddBoolToObject(row, "has_token", entry->token[0] != '\0'));
    }
    if (ok && (include_tokens || (cJSON_AddNumberToObject(root, "version", servers->version) &&
                                  cJSON_AddNumberToObject(root, "limit", CAP_MCP_SERVER_LIMIT)))) {
        return root;
    }
    cJSON_Delete(root);
    return NULL;
}

static bool mcp_number(const cJSON *item, uint32_t *number)
{
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 0 && item->valuedouble <= UINT32_MAX)) {
        return false;
    }
    *number = (uint32_t)item->valuedouble;
    return item->valuedouble == *number;
}

static esp_err_t mcp_decode_servers(const cJSON *root, const cap_mcp_servers_t *previous, cap_mcp_servers_t *next)
{
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "servers");
    if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) > CAP_MCP_SERVER_LIMIT) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t new_id = 0;
    for (size_t i = 0; previous && i < previous->count; ++i) {
        if (previous->items[i].id > new_id) {
            new_id = previous->items[i].id;
        }
    }
    const cJSON *row;
    cJSON_ArrayForEach(row, items) {
        cap_mcp_server_config_t *entry = &next->items[next->count++];
        const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(row, "id");
        const cJSON *token_item = cJSON_GetObjectItemCaseSensitive(row, "bearer_token");
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "name"));
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "server_url"));
        const char *token = cJSON_GetStringValue(token_item);
        if (!name || strlen(name) >= sizeof(entry->name) || !url || strlen(url) >= sizeof(entry->url) ||
                (token_item && (!token || strlen(token) >= sizeof(entry->token)))) {
            return ESP_ERR_INVALID_ARG;
        }
        if (id_item) {
            if (!mcp_number(id_item, &entry->id) || !entry->id) {
                return ESP_ERR_INVALID_ARG;
            }
            if (previous) {
                size_t i = 0;
                while (i < previous->count && previous->items[i].id != entry->id) {
                    ++i;
                }
                if (i == previous->count) {
                    return ESP_ERR_INVALID_ARG;
                }
                if (!token && strcmp(url, previous->items[i].url) == 0) {
                    token = previous->items[i].token;
                }
            }
        } else if (!previous || !(entry->id = ++new_id)) {
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(entry->name, name, sizeof(entry->name));
        strlcpy(entry->url, url, sizeof(entry->url));
        strlcpy(entry->token, token ? token : "", sizeof(entry->token));
    }
    return cap_mcp_client_validate_servers(next);
}

static esp_err_t app_config_load_mcp_servers(void)
{
    char *record = malloc(APP_MCP_SERVERS_RECORD_SIZE);
    if (!record) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = settings_store_get_string(APP_MCP_SERVERS_KEY, record, APP_MCP_SERVERS_RECORD_SIZE, "");
    if (err == ESP_OK && record[0]) {
        cap_mcp_servers_t *servers = calloc(1, sizeof(*servers));
        cJSON *root = cJSON_Parse(record);
        err = servers ? mcp_decode_servers(root, NULL, servers) : ESP_ERR_NO_MEM;
        if (err == ESP_OK) {
            err = cap_mcp_client_set_servers(servers);
        }
        cJSON_Delete(root);
        free(servers);
    }
    free(record);
    return err;
}

static esp_err_t mcp_store_servers(const cap_mcp_servers_t *servers)
{
    esp_err_t err = cap_mcp_client_validate_servers(servers);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = mcp_servers_json(servers, true);
    char *record = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (!record) {
        return ESP_ERR_NO_MEM;
    }
    err = strlen(record) < APP_MCP_SERVERS_RECORD_SIZE ?
          settings_store_set_string(APP_MCP_SERVERS_KEY, record) : ESP_ERR_INVALID_SIZE;
    cJSON_free(record);
    return err == ESP_OK ? cap_mcp_client_set_servers(servers) : err;
}

cJSON *app_config_mcp_servers(void)
{
    cap_mcp_servers_t *servers = malloc(sizeof(*servers));
    if (!servers) {
        return NULL;
    }
    cap_mcp_client_get_servers(servers);
    cJSON *root = mcp_servers_json(servers, false);
    free(servers);
    return root;
}

esp_err_t app_config_mcp_replace(const cJSON *input)
{
    uint32_t version;
    if (!mcp_number(cJSON_GetObjectItemCaseSensitive(input, "version"), &version)) {
        return ESP_ERR_INVALID_ARG;
    }
    cap_mcp_servers_t *before = malloc(sizeof(*before));
    cap_mcp_servers_t *after = calloc(1, sizeof(*after));
    esp_err_t err = ESP_ERR_NO_MEM;
    if (before && after) {
        cap_mcp_client_get_servers(before);
        err = before->version == version ? mcp_decode_servers(input, before, after) : ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) {
            err = mcp_store_servers(after);
        }
    }
    free(before);
    free(after);
    return err;
}

esp_err_t app_config_mcp_update_default(const char *url, const char *token)
{
    if (!cap_mcp_client_get_named_default(NULL, NULL, 0)) {
        return ESP_OK;
    }
    cap_mcp_servers_t *servers = malloc(sizeof(*servers));
    if (!servers) {
        return ESP_ERR_NO_MEM;
    }
    cap_mcp_client_get_servers(servers);
    cap_mcp_server_config_t *entry = &servers->items[0];
    esp_err_t err = ESP_OK;
    if (url[0] && !servers->count) {
        memset(entry, 0, sizeof(*entry));
        entry->id = 1;
        strlcpy(entry->name, "MCP", sizeof(entry->name));
        servers->count = 1;
    }
    if (servers->count && (strcmp(entry->url, url) != 0 || strcmp(entry->token, token) != 0)) {
        if (url[0]) {
            strlcpy(entry->url, url, sizeof(entry->url));
            strlcpy(entry->token, token, sizeof(entry->token));
        } else {
            --servers->count;
            memmove(entry, entry + 1, servers->count * sizeof(*entry));
            memset(entry + servers->count, 0, sizeof(*entry));
        }
        err = mcp_store_servers(servers);
    }
    free(servers);
    return err;
}

static esp_err_t app_config_load_mcp_bearer(app_config_t *config)
{
    if (!config->mcp_server_url[0]) {
        return ESP_OK;
    }
    char *record = malloc(APP_MCP_BEARER_RECORD_SIZE);
    if (!record) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = settings_store_get_string(APP_MCP_BEARER_KEY, record, APP_MCP_BEARER_RECORD_SIZE, "");
    if (err == ESP_OK) {
        size_t url_len = strlen(config->mcp_server_url);
        if (strncmp(record, config->mcp_server_url, url_len) == 0 && record[url_len] == '\n') {
            strlcpy(config->mcp_bearer_token, record + url_len + 1, sizeof(config->mcp_bearer_token));
        }
    }
    free(record);
    return err;
}

static esp_err_t app_config_save_mcp_bearer(const app_config_t *config)
{
    if (!config->mcp_bearer_token[0]) {
        return settings_store_erase_key(APP_MCP_BEARER_KEY);
    }
    char *record = NULL;
    if (asprintf(&record, "%s\n%s", config->mcp_server_url, config->mcp_bearer_token) < 0) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = settings_store_set_string(APP_MCP_BEARER_KEY, record);
    free(record);
    return err;
}
#endif

esp_err_t app_config_load(app_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_load_defaults(config);

    for (size_t i = 0; i < sizeof(s_fields) / sizeof(s_fields[0]); ++i) {
        esp_err_t err = settings_store_get_string(s_fields[i].key,
                                                  app_config_field_ptr(config, &s_fields[i]),
                                                  s_fields[i].size,
                                                  s_fields[i].default_value);
        if (err != ESP_OK) {
            return err;
        }
    }

    for (size_t i = 0; i < sizeof(s_fields) / sizeof(s_fields[0]); ++i) {
        bool exists = false;

        if (strcmp(s_fields[i].key, "http_allow_ls") != 0) {
            continue;
        }

        esp_err_t err = settings_store_has_key(s_fields[i].key, &exists);
        if (err != ESP_OK) {
            return err;
        }
        if (exists) {
            continue;
        }

        err = settings_store_set_string(s_fields[i].key,
                                        app_config_field_ptr(config, &s_fields[i]));
        if (err != ESP_OK) {
            return err;
        }
    }

#if CONFIG_APP_CLAW_CAP_MCP_CLIENT
    if (cap_mcp_client_get_named_default(config->mcp_server_url, config->mcp_bearer_token,
                                         sizeof(config->mcp_server_url))) {
        return ESP_OK;
    }
    return app_config_load_mcp_bearer(config);
#else
    return ESP_OK;
#endif
}

esp_err_t app_config_save(const app_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_APP_CLAW_CAP_MCP_CLIENT
    bool named_mcp = cap_mcp_client_get_named_default(NULL, NULL, 0);
#endif
    for (size_t i = 0; i < sizeof(s_fields) / sizeof(s_fields[0]); ++i) {
#if CONFIG_APP_CLAW_CAP_MCP_CLIENT
        if (named_mcp && s_fields[i].offset == offsetof(app_config_t, mcp_server_url)) {
            continue;
        }
#endif
        esp_err_t err = settings_store_set_string(s_fields[i].key,
                                                  app_config_field_cptr(config, &s_fields[i]));
        if (err != ESP_OK) {
            return err;
        }
    }

#if CONFIG_APP_CLAW_CAP_MCP_CLIENT
    if (!named_mcp) {
        esp_err_t err = app_config_save_mcp_bearer(config);
        if (err != ESP_OK) {
            return err;
        }
    }
#endif
    return settings_store_commit();
}

esp_err_t app_config_validate_wifi(const app_config_t *config, const char **message)
{
    if (message) {
        *message = NULL;
    }
    if (!config) {
        if (message) {
            *message = "Missing Wi-Fi configuration";
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (config->wifi_ssid[0] != '\0' && strlen(config->wifi_ssid) >= 32) {
        if (message) {
            *message = "wifi_ssid must be 1-31 characters";
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (config->wifi_password[0] != '\0') {
        size_t wifi_password_len = strlen(config->wifi_password);
        if (wifi_password_len < 8 || wifi_password_len > 63) {
            if (message) {
                *message = "wifi_password must be empty or 8-63 characters";
            }
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (config->ap_password[0] != '\0') {
        size_t ap_password_len = strlen(config->ap_password);
        if (ap_password_len < 8 || ap_password_len > 63) {
            if (message) {
                *message = "ap_password must be empty or 8-63 characters";
            }
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (config->ap_ssid[0] != '\0' && strlen(config->ap_ssid) > 32) {
        if (message) {
            *message = "ap_ssid must be 1-32 characters";
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (!app_config_ap_behavior_is_valid(config->ap_behavior)) {
        if (message) {
            *message = "ap_behavior must be keep or close_on_sta";
        }
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

bool app_config_show_tool_progress(const app_config_t *config)
{
    return strcmp(config->show_tool_progress, "true") == 0 || strcmp(config->show_tool_progress, "1") == 0;
}

int app_config_parse_sticker_rate(const char *value)
{
    if (!value || !*value || strlen(value) > 3 || value[strspn(value, "0123456789")]) {
        return -1;
    }
    int rate = atoi(value);
    return rate <= 100 ? rate : -1;
}

unsigned app_config_auto_sticker_rate(const app_config_t *config)
{
    if (strcmp(config->auto_stickers, "true") != 0 && strcmp(config->auto_stickers, "1") != 0) {
        return 0;
    }
    int rate = app_config_parse_sticker_rate(config->sticker_rate);
    return rate >= 0 ? (unsigned)rate : (unsigned)atoi(APP_DEFAULT_STICKER_RATE);
}

void app_config_to_claw(const app_config_t *config, app_claw_config_t *out)
{
    if (!config || !out) {
        return;
    }

    memset(out, 0, sizeof(*out));

    strlcpy(out->llm_api_key, config->llm_api_key, sizeof(out->llm_api_key));
    strlcpy(out->llm_backend_type, config->llm_backend_type, sizeof(out->llm_backend_type));
    strlcpy(out->llm_model, config->llm_model, sizeof(out->llm_model));
    strlcpy(out->llm_base_url, config->llm_base_url, sizeof(out->llm_base_url));
    strlcpy(out->llm_auth_type, config->llm_auth_type, sizeof(out->llm_auth_type));
    strlcpy(out->llm_timeout_ms, config->llm_timeout_ms, sizeof(out->llm_timeout_ms));
    strlcpy(out->llm_max_tokens, config->llm_max_tokens, sizeof(out->llm_max_tokens));
    strlcpy(out->llm_context_window, config->llm_context_window, sizeof(out->llm_context_window));
    strlcpy(out->llm_default_image_max_bytes,
            config->llm_default_image_max_bytes,
            sizeof(out->llm_default_image_max_bytes));
    strlcpy(out->llm_max_tokens_field, config->llm_max_tokens_field, sizeof(out->llm_max_tokens_field));
    strlcpy(out->llm_supports_tools, config->llm_supports_tools, sizeof(out->llm_supports_tools));
    strlcpy(out->llm_supports_vision, config->llm_supports_vision, sizeof(out->llm_supports_vision));
    strlcpy(out->llm_image_remote_url_only,
            config->llm_image_remote_url_only,
            sizeof(out->llm_image_remote_url_only));
    strlcpy(out->qq_app_id, config->qq_app_id, sizeof(out->qq_app_id));
    strlcpy(out->qq_app_secret, config->qq_app_secret, sizeof(out->qq_app_secret));
    strlcpy(out->qq_msg_type, config->qq_msg_type, sizeof(out->qq_msg_type));
    strlcpy(out->feishu_app_id, config->feishu_app_id, sizeof(out->feishu_app_id));
    strlcpy(out->feishu_app_secret, config->feishu_app_secret, sizeof(out->feishu_app_secret));
    strlcpy(out->tg_bot_token, config->tg_bot_token, sizeof(out->tg_bot_token));
    strlcpy(out->wechat_token, config->wechat_token, sizeof(out->wechat_token));
    strlcpy(out->wechat_base_url, config->wechat_base_url, sizeof(out->wechat_base_url));
    strlcpy(out->wechat_cdn_base_url, config->wechat_cdn_base_url, sizeof(out->wechat_cdn_base_url));
    strlcpy(out->wechat_account_id, config->wechat_account_id, sizeof(out->wechat_account_id));
    strlcpy(out->search_brave_key, config->search_brave_key, sizeof(out->search_brave_key));
    strlcpy(out->search_tavily_key, config->search_tavily_key, sizeof(out->search_tavily_key));
    strlcpy(out->search_http_allowlist,
            config->search_http_allowlist,
            sizeof(out->search_http_allowlist));
    strlcpy(out->enabled_cap_groups, config->enabled_cap_groups, sizeof(out->enabled_cap_groups));
    strlcpy(out->llm_visible_cap_groups, config->llm_visible_cap_groups, sizeof(out->llm_visible_cap_groups));
    strlcpy(out->mcp_server_url, config->mcp_server_url, sizeof(out->mcp_server_url));
    strlcpy(out->mcp_bearer_token, config->mcp_bearer_token, sizeof(out->mcp_bearer_token));
    strlcpy(out->enabled_lua_modules, config->enabled_lua_modules, sizeof(out->enabled_lua_modules));
}

const char *app_config_get_timezone(const app_config_t *config)
{
    return config ? config->time_timezone : NULL;
}
