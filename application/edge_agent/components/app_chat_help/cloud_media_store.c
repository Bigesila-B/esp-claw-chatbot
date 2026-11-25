/* SPDX-License-Identifier: Apache-2.0 */
#include "cloud_media_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define MEDIA_PROFILES_KEY "profiles_v1"
#define MEDIA_PROFILES_MAX 8
#define MEDIA_STORE_MAX (16 * 1024)
static QueueHandle_t s_storage_queue;
static nvs_handle_t s_nvs;

typedef struct {
    esp_err_t (*run)(void *);
    void *argument;
    SemaphoreHandle_t done;
    esp_err_t result;
} storage_job_t;

static void storage_worker(void *argument)
{
    (void)argument;
    storage_job_t *job;
    while (xQueueReceive(s_storage_queue, &job, portMAX_DELAY) == pdTRUE) {
        job->result = job->run(job->argument);
        xSemaphoreGive(job->done);
    }
    vTaskDelete(NULL);
}

esp_err_t app_cloud_media_storage_call(esp_err_t (*run)(void *), void *argument)
{
    if (!s_storage_queue) return ESP_ERR_INVALID_STATE;
    storage_job_t job = { .run = run, .argument = argument, .done = xSemaphoreCreateBinary() };
    if (!job.done) return ESP_ERR_NO_MEM;
    storage_job_t *pending = &job;
    if (xQueueSend(s_storage_queue, &pending, portMAX_DELAY) == pdTRUE) {
        xSemaphoreTake(job.done, portMAX_DELAY);
    } else {
        job.result = ESP_ERR_TIMEOUT;
    }
    vSemaphoreDelete(job.done);
    return job.result;
}

const char *cloud_media_string(const cJSON *object, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
}

bool cloud_media_url_valid(const char *url)
{
    if (!url || strlen(url) > CLOUD_MEDIA_URL_MAX || strpbrk(url, "\r\n\t <>\\#")) {
        return false;
    }
    size_t scheme = strncmp(url, "https://", 8) == 0 ? 8 : strncmp(url, "http://", 7) == 0 ? 7 : 0;
    size_t authority = scheme ? strcspn(url + scheme, "/?") : 0;
    return authority && !memchr(url + scheme, '@', authority) && !memchr(url + scheme, '{', authority);
}

static bool secret_name(const char *name)
{
    static const char *const keys[] = {"authorization", "api_key", "apikey", "api-key", "access_token",
                                      "token", "bearer", "password", "secret", "client_secret", "x-api-key"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (strcasecmp(name, keys[i]) == 0) return true;
    }
    return false;
}

static bool parameters_valid(const cJSON *object, bool scalar, unsigned depth)
{
    if (depth > 8) return false;
    const cJSON *item;
    cJSON_ArrayForEach(item, object) {
        if (item->string && (secret_name(item->string) || (scalar && strpbrk(item->string, "\"\\\r\n")))) return false;
        if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
            if (scalar || !parameters_valid(item, false, depth + 1)) return false;
        } else if (scalar && cJSON_IsString(item) && strpbrk(item->valuestring, "\r\n")) {
            return false;
        }
    }
    return true;
}

esp_err_t cloud_media_store_init(void)
{
    if (s_storage_queue) {
        return ESP_OK;
    }
    esp_err_t err = nvs_open("claw_media", NVS_READWRITE, &s_nvs);
    if (err == ESP_OK) {
        /* LLM and Web tasks can run on PSRAM stacks, which cannot access flash directly. */
        s_storage_queue = xQueueCreate(4, sizeof(storage_job_t *));
        if (!s_storage_queue || xTaskCreate(storage_worker, "claw_storage", 4608, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
            if (s_storage_queue) vQueueDelete(s_storage_queue);
            s_storage_queue = NULL;
            nvs_close(s_nvs);
            err = ESP_ERR_NO_MEM;
        }
    }
    return err;
}

static esp_err_t load_profiles(cJSON **profiles)
{
    size_t size = 0;
    esp_err_t err = nvs_get_blob(s_nvs, MEDIA_PROFILES_KEY, NULL, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *profiles = cJSON_CreateArray();
        return *profiles ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK || size == 0 || size > MEDIA_STORE_MAX) {
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    char *text = malloc(size);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_blob(s_nvs, MEDIA_PROFILES_KEY, text, &size);
    *profiles = err == ESP_OK && size > 0 && text[size - 1] == '\0' ? cJSON_Parse(text) : NULL;
    memset(text, 0, size);
    free(text);
    return err != ESP_OK ? err : cJSON_IsArray(*profiles) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t save_profiles(const cJSON *profiles)
{
    char *text = cJSON_PrintUnformatted(profiles);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    size_t size = strlen(text);
    esp_err_t err = size < MEDIA_STORE_MAX ? nvs_set_blob(s_nvs, MEDIA_PROFILES_KEY, text, size + 1) : ESP_ERR_INVALID_SIZE;
    memset(text, 0, size);
    cJSON_free(text);
    return err == ESP_OK ? nvs_commit(s_nvs) : err;
}

static bool valid_name(const char *name)
{
    if (!name || !*name || strlen(name) > CLOUD_MEDIA_NAME_MAX) {
        return false;
    }
    for (; *name; ++name) {
        if (!isalnum((unsigned char)*name) && *name != '-' && *name != '_') {
            return false;
        }
    }
    return true;
}

static int find_profile(const cJSON *profiles, const char *name)
{
    int index = 0;
    const cJSON *profile;
    cJSON_ArrayForEach(profile, profiles) {
        const char *key = cloud_media_string(profile, "name");
        if (key && strcmp(key, name) == 0) {
            return index;
        }
        index++;
    }
    return -1;
}

static const struct {
    const char *key;
    size_t max;
} s_fields[] = {
    {"model_field", 64}, {"input_field", 64}, {"result_path", 128},
    {"auth_header", 64}, {"auth_prefix", 32}, {"job_path", 128},
    {"poll_url", 512}, {"status_path", 128}, {"success_value", 64}, {"failure_value", 64},
};

static esp_err_t build_profile(const cJSON *input, bool draft, cJSON **output)
{
    const char *name = cloud_media_string(input, "name");
    const char *kind = cloud_media_string(input, "kind");
    const char *model = cloud_media_string(input, "model");
    const char *url = cloud_media_string(input, "url");
    const cJSON *extra = cJSON_GetObjectItemCaseSensitive(input, "parameters");
    if (!valid_name(name) || !kind || (strcmp(kind, "image") && strcmp(kind, "tts") && strcmp(kind, "stt")) ||
            !model || !*model || strlen(model) > 128 || strpbrk(model, "\r\n") ||
            !cloud_media_url_valid(url) || strlen(url) > 512 || (draft && strcmp(kind, "stt") == 0) ||
            (extra && (!cJSON_IsObject(extra) || !parameters_valid(extra, strcmp(kind, "stt") == 0, 0)))) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *poll = cloud_media_string(input, "poll_url");
    if (poll && *poll && (!cloud_media_url_valid(poll) || !strstr(poll, "{id}") || strcmp(kind, "stt") == 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *header = cloud_media_string(input, "auth_header");
    if (header && (strspn(header, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != strlen(header) ||
                   !strcasecmp(header, "Host") || !strcasecmp(header, "Content-Type") ||
                   !strcasecmp(header, "Content-Length") || !strcasecmp(header, "Transfer-Encoding") ||
                   !strcasecmp(header, "Connection"))) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *profile = cJSON_CreateObject();
    *output = profile;
    if (!profile || !cJSON_AddStringToObject(profile, "name", name) ||
            !cJSON_AddStringToObject(profile, "kind", kind) || !cJSON_AddStringToObject(profile, "model", model) ||
            !cJSON_AddStringToObject(profile, "url", url) ||
            !cJSON_AddBoolToObject(profile, "enabled", !draft && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input, "enabled")))) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < sizeof(s_fields) / sizeof(s_fields[0]); i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(input, s_fields[i].key);
        if (!item) {
            continue;
        }
        const char *value = cJSON_GetStringValue(item);
        if (!value || strlen(value) > s_fields[i].max || strpbrk(value, "\r\n")) {
            return ESP_ERR_INVALID_ARG;
        }
        if (i < 2 && (secret_name(value) || (strcmp(kind, "stt") == 0 && strpbrk(value, "\"\\")))) return ESP_ERR_INVALID_ARG;
        if (*value && !cJSON_AddStringToObject(profile, s_fields[i].key, value)) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (extra) {
        char *encoded = cJSON_PrintUnformatted(extra);
        size_t size = encoded ? strlen(encoded) : 0;
        cJSON_free(encoded);
        if (!size || size > 1024) {
            return size ? ESP_ERR_INVALID_SIZE : ESP_ERR_NO_MEM;
        }
        cJSON *copy = cJSON_Duplicate(extra, true);
        if (!copy || !cJSON_AddItemToObjectCS(profile, "parameters", copy)) {
            cJSON_Delete(copy);
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static bool same_origin(const char *left, const char *right)
{
    if (!cloud_media_url_valid(left) || !cloud_media_url_valid(right)) {
        return false;
    }
    const char *a = strstr(left, "://") + 3;
    const char *b = strstr(right, "://") + 3;
    size_t size = a - left + strcspn(a, "/?");
    return size == (size_t)(b - right) + strcspn(b, "/?") && strncasecmp(left, right, size) == 0;
}

typedef struct {
    const cJSON *input;
    const char *name;
    const char *kind;
    cJSON *output;
    bool draft;
    bool chat_setup;
} profile_operation_t;

static esp_err_t save_operation(void *argument)
{
    profile_operation_t *op = argument;
    const cJSON *input = op->input;
    bool draft = op->draft;
    cJSON *profile = NULL, *profiles = NULL;
    esp_err_t err = build_profile(input, draft, &profile);
    const char *name = cloud_media_string(profile, "name");
    const char *url = cloud_media_string(profile, "url");
    const char *poll = cloud_media_string(profile, "poll_url");
    const cJSON *key_field = cJSON_GetObjectItemCaseSensitive(input, "api_key");
    const char *key = cJSON_GetStringValue(key_field);
    if (err == ESP_OK && ((poll && !same_origin(url, poll)) ||
            (key_field && (draft || !key || strlen(key) > 512 || strpbrk(key, "\r\n"))))) {
        err = ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK) {
        err = load_profiles(&profiles);
        if (err == ESP_OK) {
            int index = find_profile(profiles, name);
            const cJSON *previous = index < 0 ? NULL : cJSON_GetArrayItem(profiles, index);
            const char *previous_kind = cloud_media_string(previous, "kind");
            if (previous && (draft || (op->chat_setup &&
                    (!previous_kind || strcmp(previous_kind, "tts") != 0)))) {
                err = ESP_ERR_INVALID_STATE;
            } else if (!previous && cJSON_GetArraySize(profiles) >= MEDIA_PROFILES_MAX) {
                err = ESP_ERR_INVALID_SIZE;
            } else {
                if (!key_field && previous && same_origin(url, cloud_media_string(previous, "url"))) {
                    key = cloud_media_string(previous, "api_key");
                }
                if (key && *key && !cJSON_AddStringToObject(profile, "api_key", key)) {
                    err = ESP_ERR_NO_MEM;
                } else {
                    if (op->chat_setup && previous) cJSON_DeleteItemFromArray(profiles, index);
                    bool saved = op->chat_setup ? cJSON_InsertItemInArray(profiles, 0, profile) :
                                 index < 0 ? cJSON_AddItemToArray(profiles, profile) :
                                             cJSON_ReplaceItemViaPointer(profiles, (cJSON *)previous, profile);
                    if (saved) {
                        profile = NULL;
                        err = save_profiles(profiles);
                    } else {
                        err = ESP_ERR_NO_MEM;
                    }
                }
            }
        }
    }
    cJSON_Delete(profile);
    cJSON_Delete(profiles);
    return err;
}

esp_err_t cloud_media_save_chat_tts(const cJSON *input)
{
    profile_operation_t op = { .input = input, .chat_setup = true };
    return app_cloud_media_storage_call(save_operation, &op);
}

esp_err_t app_cloud_media_save(const cJSON *input, bool draft)
{
    profile_operation_t op = { .input = input, .draft = draft };
    return app_cloud_media_storage_call(save_operation, &op);
}

static esp_err_t remove_operation(void *argument)
{
    const char *name = argument;
    if (!valid_name(name)) return ESP_ERR_INVALID_ARG;
    cJSON *profiles = NULL;
    esp_err_t err = load_profiles(&profiles);
    if (err == ESP_OK) {
        int index = find_profile(profiles, name);
        if (index < 0) {
            err = ESP_ERR_NOT_FOUND;
        } else {
            cJSON_DeleteItemFromArray(profiles, index);
            err = save_profiles(profiles);
        }
    }
    cJSON_Delete(profiles);
    return err;
}

esp_err_t app_cloud_media_remove(const char *name)
{
    return app_cloud_media_storage_call(remove_operation, (void *)name);
}

static esp_err_t list_operation(void *argument)
{
    cJSON *profiles = NULL;
    esp_err_t err = load_profiles(&profiles);
    if (err != ESP_OK) {
        cJSON_Delete(profiles);
        return err;
    }
    cJSON *profile;
    cJSON_ArrayForEach(profile, profiles) {
        bool set = cJSON_GetObjectItemCaseSensitive(profile, "api_key") != NULL;
        cJSON_DeleteItemFromObjectCaseSensitive(profile, "api_key");
        cJSON_AddBoolToObject(profile, "api_key_set", set);
    }
    *(cJSON **)argument = profiles;
    return ESP_OK;
}

cJSON *app_cloud_media_profiles(void)
{
    cJSON *profiles = NULL;
    (void)app_cloud_media_storage_call(list_operation, &profiles);
    return profiles;
}

static esp_err_t select_operation(void *argument)
{
    profile_operation_t *op = argument;
    cJSON *profiles = NULL, *selected = NULL;
    esp_err_t err = load_profiles(&profiles);
    if (err == ESP_OK) {
        cJSON *profile;
        cJSON_ArrayForEach(profile, profiles) {
            const char *type = cloud_media_string(profile, "kind");
            const char *key = cloud_media_string(profile, "name");
            if (type && key && strcmp(type, op->kind) == 0 && (!op->name || !*op->name || strcmp(key, op->name) == 0) &&
                    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(profile, "enabled"))) {
                selected = cJSON_DetachItemViaPointer(profiles, profile);
                break;
            }
        }
    }
    cJSON_Delete(profiles);
    op->output = selected;
    return err;
}

cJSON *cloud_media_select(const char *kind, const char *name)
{
    profile_operation_t op = { .kind = kind, .name = name };
    (void)app_cloud_media_storage_call(select_operation, &op);
    return op.output;
}
