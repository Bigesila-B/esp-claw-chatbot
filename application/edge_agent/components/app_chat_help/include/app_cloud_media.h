/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "cJSON.h"
#include "claw_event.h"
#include "claw_core.h"
#include "esp_err.h"
#include <stdbool.h>

esp_err_t app_cloud_media_init(void);
void app_cloud_media_set_reply_mode(int mode);
bool app_cloud_media_voice_only(void);
char *app_cloud_media_display_reply(claw_core_handle_t core, const claw_core_request_t *request, const char *text);
bool app_cloud_media_deliver_reply(const claw_event_t *event);
esp_err_t app_cloud_media_storage_call(esp_err_t (*run)(void *), void *argument);
cJSON *app_cloud_media_profiles(void);
esp_err_t app_cloud_media_save(const cJSON *profile, bool draft);
esp_err_t app_cloud_media_remove(const char *name);
bool app_cloud_media_is_setup_message(const char *text);
bool app_cloud_media_handle_event(const claw_event_t *event, esp_err_t *status);
bool app_cloud_media_is_audio_file(const char *path);
esp_err_t app_cloud_media_web_transcribe(const char *session, const char *message_id,
                                        const char *path, const char *caption, bool reply_to_source);
