/* SPDX-License-Identifier: Apache-2.0 */
#include "cloud_media_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "claw_paths.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

#define MEDIA_REPLY_MAX (16 * 1024)
#define TTS_BASE64_MAX (((CLOUD_MEDIA_TTS_MAX + 2) / 3) * 4)
#define TTS_DECODE_BLOCK 1024
#define MEDIA_HTTP_TIMEOUT_MS 25000
#define MEDIA_POLL_MS 1800
#define MEDIA_POLL_WINDOW_US (75 * 1000000LL)
#define MEDIA_BOUNDARY "espclaw-cloud-media"

static const char *setting(const cJSON *profile, const char *key, const char *fallback)
{
    const char *text = cloud_media_string(profile, key);
    return text && *text ? text : fallback;
}

static bool is_mimo_model(const cJSON *profile)
{
    const char *model = setting(profile, "model", "");
    return !strcmp(setting(profile, "kind", ""), "tts") &&
           !strncasecmp(model, "mimo-", 5) && strstr(model, "-tts");
}

static bool is_mimo_tts(const cJSON *profile, const char *url)
{
    static const char chat_path[] = "/chat/completions";
    size_t length = strcspn(url, "?");
    size_t suffix = sizeof(chat_path) - 1;
    return is_mimo_model(profile) && length >= suffix && !memcmp(url + length - suffix, chat_path, suffix);
}

static const cJSON *json_path(const cJSON *root, const char *path)
{
    while (root && path && *path) {
        size_t size = strcspn(path, ".");
        char name[65];
        if (!size || size >= sizeof(name)) {
            return NULL;
        }
        memcpy(name, path, size);
        name[size] = '\0';
        if (cJSON_IsArray(root)) {
            if (size > 4 || strspn(name, "0123456789") != size) {
                return NULL;
            }
            root = cJSON_GetArrayItem(root, atoi(name));
        } else {
            root = cJSON_GetObjectItemCaseSensitive(root, name);
        }
        path += size;
        if (*path && !*++path) {
            return NULL;
        }
    }
    return root;
}

static esp_err_t write_body(esp_http_client_handle_t client, const char *data, size_t length)
{
    while (length) {
        int written = esp_http_client_write(client, data, (int)length);
        if (written <= 0) {
            return ESP_FAIL;
        }
        data += written;
        length -= written;
    }
    return ESP_OK;
}

static esp_err_t write_file(esp_http_client_handle_t client, const char *path, size_t size)
{
    FILE *file = fopen(path, "rb");
    char *buf = malloc(1024);
    esp_err_t err = !file ? ESP_ERR_NOT_FOUND : !buf ? ESP_ERR_NO_MEM : ESP_OK;
    while (err == ESP_OK && size) {
        size_t block = size < 1024 ? size : 1024;
        if (fread(buf, 1, block, file) != block) {
            err = ESP_FAIL;
            break;
        }
        err = write_body(client, buf, block);
        size -= block;
    }
    if (file) fclose(file);
    free(buf);
    return err;
}

static esp_err_t exchange(const cJSON *profile, const char *url, const char *prefix,
                          const char *file, size_t size, int timeout_ms, cJSON **response)
{
    *response = NULL;
    const char suffix[] = "\r\n--" MEDIA_BOUNDARY "--\r\n";
    size_t length = prefix ? strlen(prefix) : 0;
    esp_http_client_config_t config = {
        .url = url, .method = prefix ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = timeout_ms, .disable_auto_redirect = true,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
        .buffer_size = 1024, .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    char *auth = NULL;
    const char *key = cloud_media_string(profile, "api_key");
    const char mimo_origin[] = "https://api.xiaomimimo.com/";
    bool mimo = is_mimo_tts(profile, url);
    const char *header = setting(profile, "auth_header", mimo &&
                                 !strncasecmp(url, mimo_origin, sizeof(mimo_origin) - 1) ? "api-key" : "Authorization");
    const char *auth_prefix = setting(profile, "auth_prefix", strcasecmp(header, "Authorization") == 0 ? "Bearer " : "");
    esp_err_t err = ESP_OK;
    if (key && *key) {
        err = asprintf(&auth, "%s%s", auth_prefix, key) < 0 ? ESP_ERR_NO_MEM :
              esp_http_client_set_header(client, header, auth);
    }
    if (err == ESP_OK && prefix) {
        err = esp_http_client_set_header(client, "Content-Type", file ?
              "multipart/form-data; boundary=" MEDIA_BOUNDARY : "application/json");
    }
    if (err == ESP_OK) {
        err = esp_http_client_open(client, length + (file ? size + sizeof(suffix) - 1 : 0));
    }
    if (err == ESP_OK && prefix) err = write_body(client, prefix, length);
    if (err == ESP_OK && file) err = write_file(client, file, size);
    if (err == ESP_OK && file) err = write_body(client, suffix, sizeof(suffix) - 1);
    size_t limit = mimo ? TTS_BASE64_MAX + MEDIA_REPLY_MAX : MEDIA_REPLY_MAX;
    if (err == ESP_OK) {
        int64_t reply_size = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 401 || status == 403) err = ESP_ERR_NOT_ALLOWED;
        else if (status < 200 || status >= 300) err = ESP_ERR_INVALID_RESPONSE;
        else if (reply_size > (int64_t)limit) err = ESP_ERR_INVALID_SIZE;
        else if (reply_size > 0) limit = (size_t)reply_size;
    }
    char *body = NULL;
    if (err == ESP_OK) {
        body = mimo ? heap_caps_malloc(limit + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : malloc(limit + 1);
        if (!body && mimo && limit <= MEDIA_REPLY_MAX) body = malloc(limit + 1);
        if (!body) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        size_t used = 0;
        int count = 0;
        do {
            count = esp_http_client_read(client, body + used, limit + 1 - used);
            if (count > 0) used += count;
        } while (count > 0 && used <= limit);
        if (count < 0 || used > limit) {
            err = count < 0 ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
        } else {
            body[used] = '\0';
            *response = cJSON_ParseWithOpts(body, NULL, true);
            if (!*response) err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    free(body);
    esp_http_client_cleanup(client);
    if (auth) {
        memset(auth, 0, strlen(auth));
        free(auth);
    }
    return err;
}

static char *mimo_request(const cJSON *profile, const char *input, const char *format)
{
    const cJSON *parameters = cJSON_GetObjectItemCaseSensitive(profile, "parameters");
    cJSON *body = parameters ? cJSON_Duplicate(parameters, true) : cJSON_CreateObject();
    cJSON *audio = cJSON_GetObjectItemCaseSensitive(body, "audio");
    cJSON *messages = cJSON_GetObjectItemCaseSensitive(body, "messages");
    if (!audio) audio = cJSON_AddObjectToObject(body, "audio");
    if (!messages) messages = cJSON_AddArrayToObject(body, "messages");
    cJSON *line = cJSON_IsArray(messages) ? cJSON_CreateObject() : NULL;
    if (line && !cJSON_AddItemToArray(messages, line)) {
        cJSON_Delete(line);
        line = NULL;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(audio, "format");
    cJSON_DeleteItemFromObjectCaseSensitive(body, "model");
    cJSON_DeleteItemFromObjectCaseSensitive(body, "stream");
    char *json = cJSON_IsObject(audio) && cJSON_AddStringToObject(audio, "format", format) &&
                 cJSON_AddStringToObject(body, "model", cloud_media_string(profile, "model")) &&
                 cJSON_AddFalseToObject(body, "stream") && cJSON_AddStringToObject(line, "role", "assistant") &&
                 cJSON_AddStringToObject(line, "content", input) ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    return json;
}

static bool valid_audio_start(const unsigned char *bytes, size_t count, const char *format)
{
    if (!strcmp(format, "wav")) {
        return count >= 12 && !memcmp(bytes, "RIFF", 4) && !memcmp(bytes + 8, "WAVE", 4);
    }
    return count >= 4 && (!memcmp(bytes, "ID3", 3) || (bytes[0] == 0xff && (bytes[1] & 0xe0) == 0xe0));
}

static esp_err_t save_audio(const cJSON *response, const char *path, const char *format, char **result)
{
    const char *encoded = cJSON_GetStringValue(json_path(response, path));
    if (!encoded || !*encoded) return ESP_ERR_NOT_FOUND;
    size_t length = strlen(encoded);
    if (length > TTS_BASE64_MAX) return ESP_ERR_INVALID_SIZE;
    const char *padding = strchr(encoded, '=');
    if (length % 4 || (padding && (encoded + length - padding > 2 || padding[strspn(padding, "=")]))) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    char dir[128];
    esp_err_t err = claw_paths_join(CLAW_PATH_DATA, CLOUD_MEDIA_TTS_DIR, dir, sizeof(dir));
    if (err != ESP_OK) return err;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return ESP_ERR_NO_MEM;
    char *file_path = NULL;
    unsigned char *bytes = malloc(3 * TTS_DECODE_BLOCK / 4);
    if (!bytes || asprintf(&file_path, "%s/pending.%s", dir, format) < 0) {
        free(bytes);
        return ESP_ERR_NO_MEM;
    }
    FILE *file = fopen(file_path, "wb");
    err = file ? ESP_OK : ESP_ERR_NO_MEM;
    size_t total = 0;
    for (size_t offset = 0; err == ESP_OK && offset < length; offset += TTS_DECODE_BLOCK) {
        size_t block = length - offset < TTS_DECODE_BLOCK ? length - offset : TTS_DECODE_BLOCK;
        size_t count = 0;
        if (mbedtls_base64_decode(bytes, 3 * TTS_DECODE_BLOCK / 4, &count,
                                  (const unsigned char *)encoded + offset, block) != 0 || !count ||
                (!offset && !valid_audio_start(bytes, count, format))) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else if (total + count > CLOUD_MEDIA_TTS_MAX) {
            err = ESP_ERR_INVALID_SIZE;
        } else if (fwrite(bytes, 1, count, file) != count) {
            err = ESP_ERR_NO_MEM;
        }
        total += count;
    }
    if (file && fclose(file) != 0) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        *result = file_path;
    } else {
        unlink(file_path);
        free(file_path);
    }
    free(bytes);
    return err;
}

static char *json_request(const cJSON *profile, const char *kind, const char *input)
{
    const cJSON *parameters = cJSON_GetObjectItemCaseSensitive(profile, "parameters");
    cJSON *body = parameters ? cJSON_Duplicate(parameters, true) : cJSON_CreateObject();
    const char *model = setting(profile, "model_field", "model");
    const char *field = setting(profile, "input_field", strcmp(kind, "image") == 0 ? "prompt" : "input");
    cJSON_DeleteItemFromObjectCaseSensitive(body, model);
    cJSON_DeleteItemFromObjectCaseSensitive(body, field);
    char *json = strcmp(model, field) != 0 &&
                 cJSON_AddStringToObject(body, model, cloud_media_string(profile, "model")) &&
                 cJSON_AddStringToObject(body, field, input) ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    return json;
}

static const char *audio_extension(const char *path)
{
    const char *extension = strrchr(path, '.');
    if (!extension) return ".bin";
    const char *const known[] = {".silk", ".amr", ".aac", ".m4a", ".mp3", ".ogg", ".oga", ".opus", ".wav", ".webm", ".mp4", ".flac"};
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        if (strcasecmp(known[i], extension) == 0) return known[i];
    }
    return ".bin";
}

static char *form_request(const cJSON *profile, const char *path, const char *mime)
{
    const char *model = setting(profile, "model_field", "model");
    const char *field = setting(profile, "input_field", "file");
    if (strcmp(model, field) == 0) return NULL;
    if (!mime || strncmp(mime, "audio/", 6) || strlen(mime) > 64 || strpbrk(mime, "\r\n")) {
        mime = "application/octet-stream";
    }
    char *body = NULL;
    const cJSON *item;
    const cJSON *parameters = cJSON_GetObjectItemCaseSensitive(profile, "parameters");
    cJSON_ArrayForEach(item, parameters) {
        if (!strcmp(item->string, model) || !strcmp(item->string, field) || cJSON_IsNull(item)) continue;
        const char *value = cJSON_GetStringValue(item);
        char *scalar = value ? NULL : cJSON_PrintUnformatted(item);
        char *next = NULL;
        int count = asprintf(&next, "%s--" MEDIA_BOUNDARY "\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n",
                             body ? body : "", item->string, value ? value : scalar ? scalar : "");
        cJSON_free(scalar);
        free(body);
        if (count < 0) return NULL;
        body = next;
    }
    char *prefix = NULL;
    int count = asprintf(&prefix, "%s--" MEDIA_BOUNDARY "\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n"
                 "--" MEDIA_BOUNDARY "\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"speech%s\"\r\n"
                 "Content-Type: %s\r\n\r\n", body ? body : "", model, cloud_media_string(profile, "model"), field,
                 audio_extension(path), mime);
    free(body);
    return count >= 0 ? prefix : NULL;
}

static esp_err_t extract_result(const cJSON *response, const char *path, bool text, char **result)
{
    const char *value = cJSON_GetStringValue(json_path(response, path));
    if (!value || !*value) return ESP_ERR_NOT_FOUND;
    if (text ? strlen(value) > CLOUD_MEDIA_INPUT_MAX : !cloud_media_url_valid(value)) return ESP_ERR_INVALID_RESPONSE;
    *result = strdup(value);
    return *result ? ESP_OK : ESP_ERR_NO_MEM;
}

static char *make_poll_url(const char *pattern, const cJSON *job)
{
    char number[32];
    const char *id = cJSON_GetStringValue(job);
    if (!id && cJSON_IsNumber(job)) {
        snprintf(number, sizeof(number), "%.0f", job->valuedouble);
        id = number;
    }
    if (!id || !*id || strlen(id) > 128 || strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != strlen(id)) {
        return NULL;
    }
    const char *slot = strstr(pattern, "{id}");
    char *url = NULL;
    return slot && asprintf(&url, "%.*s%s%s", (int)(slot - pattern), pattern, id, slot + 4) >= 0 ? url : NULL;
}

static esp_err_t poll_result(const cJSON *profile, const char *url, const char *path, char **result)
{
    int64_t deadline = esp_timer_get_time() + MEDIA_POLL_WINDOW_US;
    const char *status_path = cloud_media_string(profile, "status_path");
    const char *failure = setting(profile, "failure_value", "failed");
    const char *success = setting(profile, "success_value", "completed");
    while (esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(MEDIA_POLL_MS));
        int64_t remaining = (deadline - esp_timer_get_time()) / 1000;
        if (remaining <= 0) break;
        cJSON *response = NULL;
        esp_err_t err = exchange(profile, url, NULL, NULL, 0,
                                 remaining < MEDIA_HTTP_TIMEOUT_MS ? (int)remaining : MEDIA_HTTP_TIMEOUT_MS, &response);
        if (err == ESP_OK) {
            const char *status = cJSON_GetStringValue(json_path(response, status_path));
            if (status && strcmp(status, failure) == 0) err = ESP_ERR_INVALID_RESPONSE;
            else if (!status_path || (status && !strcmp(status, success))) err = extract_result(response, path, false, result);
            else err = ESP_ERR_NOT_FOUND;
        }
        cJSON_Delete(response);
        if (err != ESP_ERR_NOT_FOUND) return err;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t cloud_media_request(const cJSON *profile, const char *input, const char *file,
                              const char *mime, char **result)
{
    *result = NULL;
    const char *kind = cloud_media_string(profile, "kind");
    const char *url = cloud_media_string(profile, "url");
    bool text = strcmp(kind, "stt") == 0;
    const char *authority = strstr(url, "://");
    const char *url_path = authority ? strchr(authority + 3, '/') : NULL;
    size_t base_length = strlen(url);
    if (base_length && url[base_length - 1] == '/') base_length--;
    bool mimo_base = is_mimo_model(profile) && url_path && !strchr(url, '?') &&
                     base_length >= (size_t)(url_path - url) + 3 &&
                     !memcmp(url + base_length - 3, "/v1", 3);
    bool mimo = mimo_base || is_mimo_tts(profile, url);
    const cJSON *parameters = cJSON_GetObjectItemCaseSensitive(profile, "parameters");
    const char *format = setting(cJSON_GetObjectItemCaseSensitive(parameters, "audio"), "format", "mp3");
    if (mimo && strcmp(format, "mp3") && strcmp(format, "wav")) return ESP_ERR_NOT_SUPPORTED;
    struct stat st = {0};
    if (text && (!file || stat(file, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > CLOUD_MEDIA_AUDIO_MAX)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *body = text ? form_request(profile, file, mime) :
                 mimo ? mimo_request(profile, input, format) : json_request(profile, kind, input);
    if (!body) return ESP_ERR_NO_MEM;
    cJSON *response = NULL;
    char *endpoint = NULL;
    esp_err_t err = mimo_base && asprintf(&endpoint, "%.*s/chat/completions", (int)base_length, url) < 0 ?
                    ESP_ERR_NO_MEM : exchange(profile, endpoint ? endpoint : url, body, file, st.st_size,
                                              MEDIA_HTTP_TIMEOUT_MS, &response);
    free(endpoint);
    cJSON_free(body);
    if (err != ESP_OK) return err;
    if (mimo) {
        err = save_audio(response, setting(profile, "result_path", "choices.0.message.audio.data"), format, result);
        cJSON_Delete(response);
        return err;
    }
    const char *path = setting(profile, "result_path", text ? "text" : strcmp(kind, "image") == 0 ? "data.0.url" : "url");
    err = extract_result(response, path, text, result);
    const char *pattern = cloud_media_string(profile, "poll_url");
    if (err == ESP_ERR_NOT_FOUND && pattern) {
        char *url = make_poll_url(pattern, json_path(response, setting(profile, "job_path", "job_id")));
        cJSON_Delete(response);
        err = url ? poll_result(profile, url, path, result) : ESP_ERR_INVALID_RESPONSE;
        free(url);
        return err;
    }
    cJSON_Delete(response);
    return err;
}
