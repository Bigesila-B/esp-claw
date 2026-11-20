/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "app_cloud_media.h"

#define CLOUD_MEDIA_NAME_MAX 48
#define CLOUD_MEDIA_INPUT_MAX 4096
#define CLOUD_MEDIA_AUDIO_MAX (512 * 1024)
#define CLOUD_MEDIA_URL_MAX 1500

const char *cloud_media_string(const cJSON *object, const char *key);
bool cloud_media_url_valid(const char *url);
esp_err_t cloud_media_store_init(void);
cJSON *cloud_media_select(const char *kind, const char *name);
esp_err_t cloud_media_request(const cJSON *profile, const char *input, const char *file,
                              const char *mime, char **result);
