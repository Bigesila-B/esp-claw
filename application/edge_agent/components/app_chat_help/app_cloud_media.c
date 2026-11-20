/* SPDX-License-Identifier: Apache-2.0 */
#include "cloud_media_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "app_capabilities.h"
#include "claw_agent_mgr.h"
#include "claw_cap.h"
#include "claw_event_router.h"
#include "claw_event_publisher.h"
#include "claw_paths.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define MEDIA_JOB_QUEUE_SIZE 3
#define MEDIA_WORKER_STACK 8192

typedef struct {
    claw_event_t event;
    cJSON *profile;
    char *file;
    char *mime;
} cloud_media_job_t;

static QueueHandle_t s_jobs;

bool app_cloud_media_is_audio_file(const char *path)
{
    if (!path) return false;
    const char *extension = strrchr(path, '.');
    if (!extension) return false;
    const char *const known[] = {".mp3", ".wav", ".ogg", ".oga", ".opus", ".m4a", ".aac", ".silk", ".amr", ".webm", ".flac"};
    for (size_t index = 0; index < sizeof(known) / sizeof(known[0]); ++index) {
        if (strcasecmp(extension, known[index]) == 0) return true;
    }
    return false;
}

static void free_job(cloud_media_job_t *job)
{
    claw_event_free(&job->event);
    cJSON_Delete(job->profile);
    free(job->file);
    free(job->mime);
    free(job);
}

static esp_err_t enqueue(const claw_event_t *event, const char *kind, const char *name,
                         const char *file, const char *mime)
{
    if (!s_jobs || !event->source_channel[0] || !event->chat_id[0]) return ESP_ERR_INVALID_STATE;
    if (event->text && strlen(event->text) > CLOUD_MEDIA_INPUT_MAX) return ESP_ERR_INVALID_SIZE;
    cloud_media_job_t *job = calloc(1, sizeof(*job));
    if (!job) return ESP_ERR_NO_MEM;
    job->profile = cloud_media_select(kind, name);
    if (!job->profile) {
        free_job(job);
        return ESP_ERR_NOT_FOUND;
    }
    job->file = file ? strdup(file) : NULL;
    job->mime = mime ? strdup(mime) : NULL;
    esp_err_t err = (file && !job->file) || (mime && !job->mime) ? ESP_ERR_NO_MEM :
                    claw_event_clone(event, &job->event);
    if (err == ESP_OK && xQueueSend(s_jobs, &job, 0) == pdTRUE) return ESP_OK;
    free_job(job);
    return err == ESP_OK ? ESP_ERR_TIMEOUT : err;
}

static esp_err_t send_media(claw_event_t *event, const char *url, bool voice)
{
    const char *channel = event->source_channel;
    const char *cap = NULL;
    if (strcmp(channel, "qq") == 0) cap = voice ? "qq_send_voice" : "qq_send_image";
    else if (strcmp(channel, "telegram") == 0) cap = voice ? "tg_send_voice" : "tg_send_image";
    if (cap) {
        cJSON *body = cJSON_CreateObject();
        char *input = cJSON_AddStringToObject(body, "path", url) ? cJSON_PrintUnformatted(body) : NULL;
        cJSON_Delete(body);
        if (!input) return ESP_ERR_NO_MEM;
        claw_cap_call_context_t context = {
            .caller = CLAW_CAP_CALLER_SYSTEM,
            .source_cap = "claw_event_router",
            .channel = channel, .chat_id = event->chat_id,
            .target_channel = channel, .target_chat_id = event->chat_id,
            .correlation_id = event->reply_to_message_id,
        };
        char output[128];
        esp_err_t err = claw_cap_call(cap, input, &context, output, sizeof(output));
        cJSON_free(input);
        if (err == ESP_OK) return ESP_OK;
    }
    if (strcmp(channel, "web") == 0) {
        strlcpy(event->event_type, "cloud_media_result", sizeof(event->event_type));
        char *message = NULL;
        if (asprintf(&message, voice ? "[ESP-Claw 语音](<%s>)" : "![生成图片](<%s>)", url) < 0) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t err = claw_event_router_reply(event, message);
        free(message);
        return err;
    }
    return claw_event_router_reply(event, url);
}

static const char *media_error(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NOT_FOUND: return "未找到已启用的模型，或接口响应缺少配置的结果字段。";
    case ESP_ERR_NOT_ALLOWED: return "云端接口拒绝鉴权，请检查 Web 模型配置中的密钥。";
    case ESP_ERR_INVALID_RESPONSE: return "云端接口请求失败，请检查接口地址、模型和响应字段。";
    case ESP_ERR_INVALID_SIZE: return "音频或接口响应超过设备处理上限。";
    case ESP_ERR_TIMEOUT: return "云端任务等待超时，请先检查服务端任务状态。";
    default: return "云端媒体请求未完成，请检查网络或设备状态。";
    }
}

static esp_err_t post_transcript(const cloud_media_job_t *job, const char *text)
{
    const claw_event_t *original = &job->event;
    const char *caption = original->text ? original->text : "";
    char *message = NULL;
    if (asprintf(&message, "[语音转写]\n%s%s%s", text, *caption ? "\n" : "", caption) < 0) return ESP_ERR_NO_MEM;
    esp_err_t err;
    if (strcmp(original->source_cap, "web_chat") == 0) {
        uint32_t run = 0;
        err = claw_agent_mgr_post_session_message(original->chat_id, message, original->message_id,
                                                 original->target_channel[0] != '\0', &run);
    } else {
        err = claw_event_router_publish_message(original->source_cap, original->source_channel, original->chat_id,
                                               message, original->sender_id,
                                               original->reply_to_message_id ? original->reply_to_message_id : original->message_id);
    }
    free(message);
    return err;
}

static void media_worker(void *ctx)
{
    (void)ctx;
    cloud_media_job_t *job;
    while (xQueueReceive(s_jobs, &job, portMAX_DELAY) == pdTRUE) {
        char *result = NULL;
        esp_err_t err = cloud_media_request(job->profile, job->event.text, job->file, job->mime, &result);
        if (err == ESP_OK) {
            const char *kind = cloud_media_string(job->profile, "kind");
            if (strcmp(kind, "stt") == 0) {
                err = post_transcript(job, result);
            } else {
                err = send_media(&job->event, result, strcmp(kind, "tts") == 0);
            }
        }
        if (job->file && strcmp(job->event.source_cap, "web_chat") == 0) (void)unlink(job->file);
        if (err != ESP_OK) (void)claw_event_router_reply(&job->event, media_error(err));
        free(result);
        free_job(job);
    }
    vTaskDelete(NULL);
}

static void make_event(claw_event_t *event, const char *channel, const char *chat, const char *text)
{
    strlcpy(event->source_cap, "cloud_media", sizeof(event->source_cap));
    strlcpy(event->source_channel, channel ? channel : "", sizeof(event->source_channel));
    strlcpy(event->chat_id, chat ? chat : "", sizeof(event->chat_id));
    strlcpy(event->event_type, "out_message", sizeof(event->event_type));
    event->text = (char *)text;
}

esp_err_t app_cloud_media_web_transcribe(const char *session, const char *message_id,
                                        const char *path, const char *caption, bool reply_to_source)
{
    claw_event_t *event = calloc(1, sizeof(*event));
    if (!event) return ESP_ERR_NO_MEM;
    make_event(event, "web", session, caption);
    strlcpy(event->source_cap, "web_chat", sizeof(event->source_cap));
    strlcpy(event->message_id, message_id, sizeof(event->message_id));
    if (reply_to_source) strlcpy(event->target_channel, "source", sizeof(event->target_channel));
    esp_err_t err = enqueue(event, "stt", NULL, path, NULL);
    free(event);
    return err;
}

static bool transcribe_attachment(const claw_event_t *event, esp_err_t *status)
{
    cJSON *payload = cJSON_Parse(event->payload_json);
    const char *mime = cloud_media_string(payload, "mime");
    const char *kind = cloud_media_string(payload, "attachment_kind");
    const char *path = cloud_media_string(payload, "saved_path");
    const char *root = claw_paths_get(CLAW_PATH_DATA);
    size_t root_size = root ? strlen(root) : 0;
    bool audio = (kind && (strcmp(kind, "voice") == 0 || strcmp(kind, "audio") == 0)) ||
                 (mime && strncmp(mime, "audio/", 6) == 0) ||
                 app_cloud_media_is_audio_file(path);
    bool valid = path && root_size && strncmp(path, root, root_size) == 0 &&
                 strncmp(path + root_size, "/inbox/", 7) == 0 && !strstr(path + root_size, "/../");
    claw_event_t *copy = audio && valid ? malloc(sizeof(*copy)) : NULL;
    esp_err_t err = audio && valid ? ESP_ERR_NO_MEM : ESP_ERR_NOT_FOUND;
    if (copy) {
        *copy = *event;
        copy->text = (char *)cloud_media_string(payload, "caption");
        err = enqueue(copy, "stt", NULL, path, mime);
        free(copy);
    }
    cJSON_Delete(payload);
    if (err == ESP_ERR_NOT_FOUND) return false;
    *status = err == ESP_OK ? err : claw_event_router_reply(event, media_error(err));
    return true;
}

bool app_cloud_media_handle_event(const claw_event_t *event, esp_err_t *status)
{
    if (strcmp(event->event_type, "attachment_saved") == 0) {
        return transcribe_attachment(event, status);
    }
    const char *text = event->text;
    if (!text) return false;
    while (isspace((unsigned char)*text)) text++;
    bool image = strncmp(text, "/image", 6) == 0 && (!text[6] || isspace((unsigned char)text[6]));
    bool voice = strncmp(text, "/tts", 4) == 0 && (!text[4] || isspace((unsigned char)text[4]));
    if (!image && !voice) return false;
    text += image ? 6 : 4;
    while (isspace((unsigned char)*text)) text++;
    const char *reply = image ? "用法 /image 提示词，或发送 /image setup 后跟接口配置 JSON。" : "用法 /tts 要朗读的文字。";
    if (image && strncmp(text, "setup", 5) == 0 && (!text[5] || isspace((unsigned char)text[5]))) {
        cJSON *input = cJSON_ParseWithOpts(text + 5, NULL, true);
        if (input && !cJSON_HasObjectItem(input, "kind")) cJSON_AddStringToObject(input, "kind", "image");
        esp_err_t err = app_cloud_media_save(input, true);
        cJSON_Delete(input);
        reply = err == ESP_OK ? "生图模型草稿已保存。打开设备 Web 的模型配置，填写密钥并启用。" :
                err == ESP_ERR_INVALID_STATE ? "模型名已存在。请在 Web 模型配置里编辑。" :
                "草稿未保存。请填写 name、url、model。密钥请在 Web 配置里填写。";
    } else if (*text) {
        claw_event_t *copy = malloc(sizeof(*copy));
        if (!copy) { *status = ESP_ERR_NO_MEM; return true; }
        *copy = *event;
        copy->text = (char *)text;
        esp_err_t err = enqueue(copy, image ? "image" : "tts", NULL, NULL, NULL);
        free(copy);
        if (err == ESP_OK) { *status = ESP_OK; return true; }
        reply = media_error(err);
    }
    *status = claw_event_router_reply(event, reply);
    return true;
}

static esp_err_t prepare_image(const char *json, const claw_cap_call_context_t *ctx, char *out, size_t size)
{
    (void)ctx;
    cJSON *input = cJSON_Parse(json);
    if (input && !cJSON_HasObjectItem(input, "kind")) cJSON_AddStringToObject(input, "kind", "image");
    esp_err_t err = app_cloud_media_save(input, true);
    cJSON_Delete(input);
    snprintf(out, size, "%s", err == ESP_OK ?
             "Draft saved. Ask the user to open the device Web model settings to enter the key and enable it." :
             err == ESP_ERR_INVALID_STATE ? "Draft name already exists. Ask the user to edit it in Web settings." :
             "Draft rejected. Supply an unused name, HTTP API URL and exact model ID, without credentials.");
    return err;
}

static esp_err_t send_from_agent(const char *json, const claw_cap_call_context_t *ctx, char *out, size_t size)
{
    cJSON *input = cJSON_Parse(json);
    const char *kind = cloud_media_string(input, "kind");
    const char *text = cloud_media_string(input, "text");
    const char *profile = cloud_media_string(input, "profile");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (ctx && kind && text && *text && (strcmp(kind, "image") == 0 || strcmp(kind, "tts") == 0)) {
        claw_event_t *event = calloc(1, sizeof(*event));
        if (!event) { cJSON_Delete(input); return ESP_ERR_NO_MEM; }
        const char *channel = ctx->target_channel && *ctx->target_channel ? ctx->target_channel : ctx->channel;
        const char *chat = ctx->target_chat_id && *ctx->target_chat_id ? ctx->target_chat_id : ctx->chat_id;
        make_event(event, channel, chat, text);
        if (ctx->channel && ctx->chat_id && channel && chat && strcmp(ctx->channel, channel) == 0 && strcmp(ctx->chat_id, chat) == 0) {
            event->reply_to_message_id = (char *)ctx->correlation_id;
        }
        err = enqueue(event, kind, profile, NULL, NULL);
        free(event);
    }
    cJSON_Delete(input);
    snprintf(out, size, "%s", err == ESP_OK ? "Task queued. The device will send the result to this chat." : media_error(err));
    return err;
}

static esp_err_t register_tools(const app_claw_config_t *config, const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    static const claw_cap_descriptor_t tools[] = {
        {
            .id = "cloud_image_setup", .name = "cloud_image_setup", .family = "cloud_media",
            .description = "Save a disabled draft for an HTTP image API from user-provided documentation. Never include API keys. Use dotted result paths such as data.0.url. A poll_url on the same origin can contain {id}.",
            .kind = CLAW_CAP_KIND_CALLABLE, .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
            .input_schema_json = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"url\":{\"type\":\"string\"},\"model\":{\"type\":\"string\"},\"model_field\":{\"type\":\"string\"},\"input_field\":{\"type\":\"string\"},\"result_path\":{\"type\":\"string\"},\"auth_header\":{\"type\":\"string\"},\"auth_prefix\":{\"type\":\"string\"},\"poll_url\":{\"type\":\"string\"},\"job_path\":{\"type\":\"string\"},\"status_path\":{\"type\":\"string\"},\"success_value\":{\"type\":\"string\"},\"failure_value\":{\"type\":\"string\"},\"parameters\":{\"type\":\"object\"}},\"required\":[\"name\",\"url\",\"model\"]}",
            .execute = prepare_image,
        },
        {
            .id = "cloud_media_send", .name = "cloud_media_send", .family = "cloud_media",
            .description = "Generate an image or read text aloud with a saved HTTP API profile. Delivers the result to the original chat. Omit profile to use the first enabled model of that kind.",
            .kind = CLAW_CAP_KIND_CALLABLE, .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
            .input_schema_json = "{\"type\":\"object\",\"properties\":{\"kind\":{\"type\":\"string\",\"enum\":[\"image\",\"tts\"]},\"profile\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"}},\"required\":[\"kind\",\"text\"]}",
            .execute = send_from_agent,
        },
    };
    static const claw_cap_group_t group = {
        .group_id = "cloud_media", .descriptors = tools, .descriptor_count = sizeof(tools) / sizeof(tools[0]),
    };
    return claw_cap_register_group(&group);
}

esp_err_t app_cloud_media_init(void)
{
    esp_err_t err = cloud_media_store_init();
    if (err != ESP_OK) return err;
    static const app_capability_external_group_t group = {
        .group_id = "cloud_media", .display_name = "Cloud media", .llm_visible_by_default = true,
        .reg = register_tools,
    };
    err = app_capabilities_register_external_group(&group);
    if (err != ESP_OK) return err;
    s_jobs = xQueueCreate(MEDIA_JOB_QUEUE_SIZE, sizeof(cloud_media_job_t *));
    if (!s_jobs || xTaskCreate(media_worker, "cloud_media", MEDIA_WORKER_STACK, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        if (s_jobs) vQueueDelete(s_jobs);
        s_jobs = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
