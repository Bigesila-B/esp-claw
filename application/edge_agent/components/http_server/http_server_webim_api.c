/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdlib.h>
#include <string.h>
#include "cap_im_local.h"
#include "app_cloud_media.h"
#include "claw_session_mgr.h"
#include "claw_event_router.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#if CONFIG_APP_CLAW_CAP_MEMORY
#include "claw_memory.h"
#endif

#define WEBIM_WS_CLIENTS 8
#define WEBIM_WS_WORK_MAX 8

typedef struct {
    httpd_handle_t server;
    int fd;
    uint32_t generation;
    char *json;
} webim_notice_t;

static void notify_client(void *arg)
{
    webim_notice_t *job = arg;
    http_server_ctx_t *ctx = http_server_ctx();
    bool valid = false;
    xSemaphoreTake(ctx->webim_lock, portMAX_DELAY);
    for (size_t i = 0; i < WEBIM_WS_CLIENTS; i++) {
        if (ctx->webim_clients[i].fd == job->fd && ctx->webim_clients[i].generation == job->generation && ctx->webim_server == job->server) valid = true;
    }
    if (ctx->webim_work_count) ctx->webim_work_count--;
    xSemaphoreGive(ctx->webim_lock);
    if (valid && httpd_ws_get_fd_info(job->server, job->fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
        const char *json = job->json ? job->json : "{\"type\":\"sessions.changed\"}";
        httpd_ws_frame_t packet = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)json, .len = strlen(json)};
        if (httpd_ws_send_frame_async(job->server, job->fd, &packet) != ESP_OK) http_server_webim_ws_fd_remove(job->fd);
    }
    free(job->json);
    free(job);
}

static esp_err_t notify_webim(const char *json)
{
    http_server_ctx_t *ctx = http_server_ctx();
    if (!ctx->webim_lock) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_OK;
    xSemaphoreTake(ctx->webim_lock, portMAX_DELAY);
    for (size_t i = 0; ctx->webim_server && i < WEBIM_WS_CLIENTS && ctx->webim_work_count < WEBIM_WS_WORK_MAX; i++) {
        if (ctx->webim_clients[i].fd < 0) continue;
        webim_notice_t *job = malloc(sizeof(*job));
        if (!job) { err = ESP_ERR_NO_MEM; break; }
        *job = (webim_notice_t){ctx->webim_server, ctx->webim_clients[i].fd,
                               ctx->webim_clients[i].generation, json ? strdup(json) : NULL};
        err = json && !job->json ? ESP_ERR_NO_MEM : httpd_queue_work(ctx->webim_server, notify_client, job);
        if (err != ESP_OK) { free(job->json); free(job); break; }
        ctx->webim_work_count++;
    }
    xSemaphoreGive(ctx->webim_lock);
    return err;
}

static void catalog_changed(void *arg)
{
    (void)arg;
    (void)notify_webim(NULL);
}

#if CONFIG_APP_CLAW_CAP_MEMORY
static esp_err_t save_outbound(void *argument)
{
    const cap_im_local_message_t *message = argument;
    claw_session_info_t *info = malloc(sizeof(*info));
    if (!info) return ESP_ERR_NO_MEM;
    esp_err_t err = claw_session_mgr_catalog_get(message->chat_id, info);
    if (err == ESP_OK) {
        const claw_core_context_record_t record = {.type = CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL, .text = message->text};
        const claw_core_context_persist_batch_t batch = {.session_id = info->session_id, .records = &record, .record_count = 1};
        err = claw_memory_persist_context_callback(&batch, NULL);
        if (err == ESP_OK) (void)claw_session_mgr_catalog_touch(info->session_id, NULL);
    }
    free(info);
    return err;
}
#endif

static esp_err_t outbound(const cap_im_local_message_t *message, void *arg)
{
    (void)arg;
    if (!message || !message->channel || strcmp(message->channel, "web") != 0) return ESP_OK;
    if (message->persisted && strlen(message->chat_id) == CLAW_SESSION_PUBLIC_ID_SIZE - 1) {
        return notify_webim(NULL);
    }
#if CONFIG_APP_CLAW_CAP_MEMORY
    if (message->save_to_history && strlen(message->chat_id) == CLAW_SESSION_PUBLIC_ID_SIZE - 1 &&
            app_cloud_media_storage_call(save_outbound, (void *)message) == ESP_OK) return notify_webim(NULL);
#endif
    cJSON *body = cJSON_CreateObject();
    if (!body || !cJSON_AddStringToObject(body, "type", "chat.message") ||
            !cJSON_AddStringToObject(body, "session", message->chat_id) ||
            !cJSON_AddStringToObject(body, "chat_id", message->chat_id) ||
            !cJSON_AddStringToObject(body, "role", "assistant") ||
            !cJSON_AddNumberToObject(body, "seq", (double)message->timestamp_ms) ||
            !cJSON_AddStringToObject(body, "text", message->text) ||
            (message->link_url && !cJSON_AddStringToObject(body, "link_url", message->link_url))) {
        cJSON_Delete(body);
        return ESP_ERR_NO_MEM;
    }
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    esp_err_t err = json ? notify_webim(json) : ESP_ERR_NO_MEM;
    free(json);
    return err;
}

void http_server_webim_ws_fd_remove(int fd)
{
    http_server_ctx_t *ctx = http_server_ctx();
    if (!ctx->webim_lock) return;
    xSemaphoreTake(ctx->webim_lock, portMAX_DELAY);
    for (size_t i = 0; i < WEBIM_WS_CLIENTS; i++) if (ctx->webim_clients[i].fd == fd) ctx->webim_clients[i].fd = -1;
    xSemaphoreGive(ctx->webim_lock);
}

void http_server_webim_stop(void)
{
    claw_session_mgr_set_change_callback(NULL, NULL);
    http_server_ctx_t *ctx = http_server_ctx();
    if (!ctx->webim_lock) return;
    xSemaphoreTake(ctx->webim_lock, portMAX_DELAY);
    ctx->webim_server = NULL;
    for (size_t i = 0; i < WEBIM_WS_CLIENTS; i++) ctx->webim_clients[i].fd = -1;
    xSemaphoreGive(ctx->webim_lock);
}

esp_err_t http_server_webim_bind_im(void)
{
    claw_session_mgr_set_change_callback(catalog_changed, NULL);
    return cap_im_local_set_outbound_callback(outbound, NULL);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return http_server_webim_error(req, ESP_ERR_NO_MEM);
    cJSON_AddBoolToObject(root, "bound", claw_event_router_channel_is_bound("web"));
    cJSON_AddStringToObject(root, "boot_id", http_server_ctx()->boot_id);
    return http_server_send_json_response(req, root);
}

static bool register_client(int fd)
{
    http_server_ctx_t *ctx = http_server_ctx();
    bool registered = false;
    xSemaphoreTake(ctx->webim_lock, portMAX_DELAY);
    int available = -1;
    for (size_t i = 0; i < WEBIM_WS_CLIENTS; i++) {
        if (ctx->webim_clients[i].fd == fd) { registered = true; break; }
        if (ctx->webim_clients[i].fd < 0 && available < 0) available = i;
    }
    if (!registered && available >= 0) {
        ctx->webim_clients[available].fd = fd;
        ctx->webim_clients[available].generation = ++ctx->webim_generation;
        registered = true;
    }
    xSemaphoreGive(ctx->webim_lock);
    return registered;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (!register_client(fd)) { httpd_sess_trigger_close(req->handle, fd); return ESP_ERR_NO_MEM; }
    if (req->method == HTTP_GET) return ESP_OK;
    httpd_ws_frame_t packet = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &packet, 0);
    if (err != ESP_OK) return err;
    if (packet.type == HTTPD_WS_TYPE_CLOSE) { http_server_webim_ws_fd_remove(fd); return ESP_OK; }
    if (packet.len > 512) { httpd_sess_trigger_close(req->handle, fd); return ESP_ERR_INVALID_SIZE; }
    uint8_t *data = calloc(1, packet.len + 1);
    if (!data) return ESP_ERR_NO_MEM;
    packet.payload = data;
    err = httpd_ws_recv_frame(req, &packet, packet.len);
    cJSON *json = err == ESP_OK ? cJSON_Parse((char *)data) : NULL;
    free(data);
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(json, "type"));
    const char *scope = cJSON_GetStringValue(cJSON_GetObjectItem(json, "scope"));
    bool subscribe = type && scope && strcmp(type, "subscribe") == 0 && strcmp(scope, "webim") == 0;
    cJSON_Delete(json);
    if (!subscribe) return err;
    http_server_ctx_t *ctx = http_server_ctx();
    char response[96];
    snprintf(response, sizeof(response), "{\"type\":\"subscribed\",\"boot_id\":\"%s\"}", ctx->boot_id);
    packet = (httpd_ws_frame_t){.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)response, .len = strlen(response)};
    return httpd_ws_send_frame(req, &packet);
}

esp_err_t http_server_register_webim_routes(httpd_handle_t server)
{
    http_server_ctx_t *ctx = http_server_ctx();
    if (!ctx->webim_lock) ctx->webim_lock = xSemaphoreCreateMutex();
    if (!ctx->webim_lock) return ESP_ERR_NO_MEM;
    ctx->webim_server = server;
    for (size_t i = 0; i < WEBIM_WS_CLIENTS; i++) ctx->webim_clients[i].fd = -1;
    static const httpd_uri_t routes[] = {
        {.uri = "/api/webim/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/webim/send", .method = HTTP_POST, .handler = http_server_webim_send_session},
        {.uri = "/ws/webim", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
