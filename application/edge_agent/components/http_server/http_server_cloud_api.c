/* SPDX-License-Identifier: Apache-2.0 */
#include "http_server_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_cloud_media.h"
#include "claw_event_router.h"
#include "sdkconfig.h"

#if CONFIG_APP_CLAW_CAP_SCHEDULER
#include <time.h>
#include "cap_scheduler.h"
#include "esp_random.h"
#endif

static const char *string(const cJSON *object, const char *key)
{
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
    return value ? value : "";
}

static esp_err_t mutation_result(httpd_req_t *req, esp_err_t status)
{
    if (status != ESP_OK) {
        httpd_err_code_t code = status == ESP_ERR_INVALID_ARG || status == ESP_ERR_INVALID_SIZE ||
                               status == ESP_ERR_INVALID_STATE ? HTTPD_400_BAD_REQUEST :
                               status == ESP_ERR_NOT_FOUND ? HTTPD_404_NOT_FOUND : HTTPD_500_INTERNAL_SERVER_ERROR;
        return httpd_resp_send_err(req, code, esp_err_to_name(status));
    }
    cJSON *result = cJSON_CreateObject();
    if (!result) return httpd_resp_send_500(req);
    cJSON_AddBoolToObject(result, "ok", true);
    return http_server_send_json_response(req, result);
}

static esp_err_t media_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        cJSON *list = app_cloud_media_profiles();
        return list ? http_server_send_json_response(req, list) : httpd_resp_send_500(req);
    }
    cJSON *body = NULL;
    esp_err_t err = http_server_parse_json_body(req, &body);
    if (err == ESP_OK) {
        const char *action = string(body, "action");
        if (!strcmp(action, "save")) err = app_cloud_media_save(cJSON_GetObjectItemCaseSensitive(body, "profile"), false);
        else if (!strcmp(action, "remove")) err = app_cloud_media_remove(string(body, "name"));
        else err = ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(body);
    return mutation_result(req, err);
}

#if CONFIG_APP_CLAW_CAP_SCHEDULER
#define WEB_REMINDER_PREFIX "web_reminder_"

static esp_err_t add_reminder(const cJSON *body)
{
    const char *text = string(body, "message");
    const char *channel = string(body, "channel");
    const char *chat = string(body, "chat_id");
    const char *kind = string(body, "kind");
    const char *cron = string(body, "cron_expr");
    double start = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(body, "start_at_ms"));
    double minutes = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(body, "minutes"));
    if (!*text || strlen(text) >= CAP_SCHEDULER_TEXT_LEN || strstr(text, "{{") ||
            !*channel || strlen(channel) >= CAP_SCHEDULER_CHANNEL_LEN || !claw_event_router_channel_is_bound(channel) ||
            !*chat || strlen(chat) >= CAP_SCHEDULER_CHAT_ID_LEN || strstr(chat, "{{")) return ESP_ERR_INVALID_ARG;
    cap_scheduler_item_t *item = calloc(1, sizeof(*item));
    if (!item) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_OK;
    if (!strcmp(kind, "once") && start > (int64_t)time(NULL) * 1000 && start < 4102444800000.0) {
        item->kind = CAP_SCHEDULER_ITEM_ONCE;
        item->start_at_ms = (int64_t)start;
    } else if (!strcmp(kind, "interval") && minutes >= 1 && minutes <= 525600) {
        item->kind = CAP_SCHEDULER_ITEM_INTERVAL;
        item->interval_ms = (int64_t)(minutes * 60000);
    } else if (!strcmp(kind, "cron") && *cron && strlen(cron) < sizeof(item->cron_expr)) {
        item->kind = CAP_SCHEDULER_ITEM_CRON;
        strlcpy(item->cron_expr, cron, sizeof(item->cron_expr));
    } else {
        free(item);
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(item->id, sizeof(item->id), WEB_REMINDER_PREFIX "%08x%08x", (unsigned)esp_random(), (unsigned)esp_random());
    item->enabled = true;
    strlcpy(item->event_type, "schedule", sizeof(item->event_type));
    strlcpy(item->event_key, item->id, sizeof(item->event_key));
    strlcpy(item->chat_id, chat, sizeof(item->chat_id));
    strlcpy(item->text, text, sizeof(item->text));
    strlcpy(item->source_channel, channel, sizeof(item->source_channel));

    cJSON *rule = cJSON_CreateObject();
    cJSON *match = cJSON_AddObjectToObject(rule, "match");
    cJSON *actions = cJSON_AddArrayToObject(rule, "actions");
    cJSON *action = cJSON_CreateObject();
    bool added = cJSON_AddItemToArray(actions, action);
    if (!added) { cJSON_Delete(action); action = NULL; }
    cJSON *input = cJSON_AddObjectToObject(action, "input");
    char *json = cJSON_AddStringToObject(rule, "id", item->id) &&
                 cJSON_AddBoolToObject(rule, "enabled", true) &&
                 cJSON_AddStringToObject(match, "event_type", "schedule") &&
                 cJSON_AddStringToObject(match, "event_key", item->id) &&
                 cJSON_AddStringToObject(action, "type", "send_message") &&
                 cJSON_AddStringToObject(input, "channel", channel) &&
                 cJSON_AddStringToObject(input, "chat_id", chat) &&
                 cJSON_AddStringToObject(input, "message", text) ? cJSON_PrintUnformatted(rule) : NULL;
    cJSON_Delete(rule);
    err = json ? claw_event_router_add_rule_json(json) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = cap_scheduler_add(item);
        if (err != ESP_OK) (void)claw_event_router_delete_rule(item->id);
    }
    cJSON_free(json);
    free(item);
    return err;
}

static esp_err_t schedule_mutation(void *argument)
{
    const cJSON *body = argument;
    const char *action = string(body, "action");
    const char *id = string(body, "id");
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(body, "enabled");
    if (!strcmp(action, "add")) return add_reminder(body);
    if (!strcmp(action, "enable") && cJSON_IsBool(enabled)) return cap_scheduler_enable(id, cJSON_IsTrue(enabled));
    if (strcmp(action, "remove") || strncmp(id, WEB_REMINDER_PREFIX, sizeof(WEB_REMINDER_PREFIX) - 1)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = cap_scheduler_remove(id);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;
    err = claw_event_router_delete_rule(id);
    return err == ESP_ERR_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t schedule_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        const size_t size = 48 * 1024;
        char *json = malloc(size);
        if (!json) return httpd_resp_send_500(req);
        esp_err_t err = cap_scheduler_list_json(json, size);
        if (err == ESP_OK) {
            httpd_resp_set_type(req, "application/json");
            err = httpd_resp_sendstr(req, json);
        } else {
            err = mutation_result(req, err);
        }
        free(json);
        return err;
    }
    cJSON *body = NULL;
    esp_err_t err = http_server_parse_json_body(req, &body);
    if (err == ESP_OK) err = app_cloud_media_storage_call(schedule_mutation, body);
    cJSON_Delete(body);
    return mutation_result(req, err);
}
#endif

esp_err_t http_server_register_cloud_routes(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/api/media-models", .method = HTTP_GET, .handler = media_handler },
        { .uri = "/api/media-models", .method = HTTP_POST, .handler = media_handler },
#if CONFIG_APP_CLAW_CAP_SCHEDULER
        { .uri = "/api/schedules", .method = HTTP_GET, .handler = schedule_handler },
        { .uri = "/api/schedules", .method = HTTP_POST, .handler = schedule_handler },
#endif
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
