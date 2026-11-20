/* SPDX-License-Identifier: Apache-2.0 */
#include "cloud_media_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define MEDIA_REPLY_MAX (16 * 1024)
#define MEDIA_HTTP_TIMEOUT_MS 25000
#define MEDIA_POLL_MS 1800
#define MEDIA_POLL_WINDOW_US (75 * 1000000LL)
#define MEDIA_BOUNDARY "espclaw-cloud-media"

static const char *setting(const cJSON *profile, const char *key, const char *fallback)
{
    const char *text = cloud_media_string(profile, key);
    return text && *text ? text : fallback;
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
    const char *header = setting(profile, "auth_header", "Authorization");
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
    if (err == ESP_OK) {
        int64_t reply_size = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 401 || status == 403) err = ESP_ERR_NOT_ALLOWED;
        else if (status < 200 || status >= 300) err = ESP_ERR_INVALID_RESPONSE;
        else if (reply_size > MEDIA_REPLY_MAX) err = ESP_ERR_INVALID_SIZE;
    }
    char *body = NULL;
    if (err == ESP_OK) {
        body = malloc(MEDIA_REPLY_MAX + 1);
        if (!body) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        size_t used = 0;
        int count = 0;
        do {
            count = esp_http_client_read(client, body + used, MEDIA_REPLY_MAX + 1 - used);
            if (count > 0) used += count;
        } while (count > 0 && used <= MEDIA_REPLY_MAX);
        if (count < 0 || used > MEDIA_REPLY_MAX) {
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
    bool text = strcmp(kind, "stt") == 0;
    struct stat st = {0};
    if (text && (!file || stat(file, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > CLOUD_MEDIA_AUDIO_MAX)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *body = text ? form_request(profile, file, mime) : json_request(profile, kind, input);
    if (!body) return ESP_ERR_NO_MEM;
    cJSON *response = NULL;
    esp_err_t err = exchange(profile, cloud_media_string(profile, "url"), body, file, st.st_size,
                             MEDIA_HTTP_TIMEOUT_MS, &response);
    cJSON_free(body);
    if (err != ESP_OK) return err;
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
