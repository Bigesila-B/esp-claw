/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "sdkconfig.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "http_parser.h"

#include "cap_mcp_client.h"
#include "cap_mcp_client_priv.h"

#define CAP_MCP_URL_SIZE         (CAP_MCP_URL_MAX_LEN + 1)
#define CAP_MCP_DIRECT_REVISION  UINT32_MAX
#define CAP_MCP_SESSION_ID_MAX   256
#define CAP_MCP_FRAME_MAX        (96 * 1024)
#define CAP_MCP_INTERNAL_FRAME_MAX (16 * 1024)
#define CAP_MCP_HTTP_TIMEOUT_MS   20000
#define CAP_MCP_TOOL_TIMEOUT_MS   45000
#define CAP_MCP_END_TIMEOUT_MS    2000
#define CAP_MCP_SSE_READ_MS       150
#define CAP_MCP_READ_SIZE        1024
#define CAP_MCP_CATALOG_PAGE_SIZE 3
#define CAP_MCP_JSON_DEPTH_MAX   32

#define CAP_MCP_STREAMABLE_VERSION "2025-11-25"
#define CAP_MCP_LEGACY_VERSION     "2024-11-05"

typedef struct {
    esp_http_client_handle_t http;
    char *session_id;
    bool sse;
    esp_err_t header_error;
} cap_mcp_http_t;

typedef struct {
    char *data;
    size_t len;
    size_t capacity;
    size_t line_start;
    bool endpoint_event;
} cap_mcp_frame_t;

typedef struct {
    cap_mcp_server_config_t server;
    uint32_t server_revision;
    char *url;
    char *authorization;
    char *session_id;
    const char *version;
    cap_mcp_http_t *legacy_stream;
    char *legacy_post_url;
} cap_mcp_session_t;

static portMUX_TYPE s_server_lock = portMUX_INITIALIZER_UNLOCKED;
static cap_mcp_servers_t s_servers;
static bool s_named_servers;
static uint32_t s_connection_revisions[CAP_MCP_SERVER_LIMIT];
static uint32_t s_next_connection_revision;
static uint32_t s_image_id_seed;

static const char *json_string(const cJSON *object, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
}

static esp_err_t parse_url(const char *url, struct http_parser_url *parsed)
{
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) ||
            strlen(url) >= CAP_MCP_URL_SIZE || strpbrk(url, "#\"\\ \t\r\n")) {
        return ESP_ERR_INVALID_ARG;
    }

    http_parser_url_init(parsed);
    if (http_parser_parse_url(url, strlen(url), 0, parsed) != 0 ||
            !(parsed->field_set & (1 << UF_HOST)) || (parsed->field_set & (1 << UF_USERINFO))) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t cap_mcp_client_validate_url(const char *url)
{
    struct http_parser_url parsed;
    return parse_url(url, &parsed);
}

esp_err_t cap_mcp_client_validate_bearer_token(const char *token)
{
    if (!token || strlen(token) > CAP_MCP_BEARER_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t body_len = strspn(token, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~+/");
    if (token[0] && (!body_len || token[body_len + strspn(token + body_len, "=")])) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t cap_mcp_client_validate_servers(const cap_mcp_servers_t *servers)
{
    if (!servers || servers->count > CAP_MCP_SERVER_LIMIT) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < servers->count; ++i) {
        const cap_mcp_server_config_t *entry = &servers->items[i];
        size_t name_len = strnlen(entry->name, sizeof(entry->name));
        if (!entry->id || !name_len || name_len >= sizeof(entry->name) ||
                entry->name[0] == ' ' || entry->name[name_len - 1] == ' ' ||
                !memchr(entry->url, '\0', sizeof(entry->url)) || !memchr(entry->token, '\0', sizeof(entry->token)) ||
                cap_mcp_client_validate_url(entry->url) != ESP_OK ||
                cap_mcp_client_validate_bearer_token(entry->token) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        for (const unsigned char *c = (const unsigned char *)entry->name; *c; ++c) {
            if (*c < 0x20 || *c == 0x7f) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        for (size_t j = 0; j < i; ++j) {
            if (entry->id == servers->items[j].id ||
                    strcasecmp(entry->name, servers->items[j].name) == 0) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

/* The catalog and connection revisions are updated together under s_server_lock. */
static uint32_t next_connection_revision(void)
{
    if (++s_next_connection_revision == CAP_MCP_DIRECT_REVISION) {
        s_next_connection_revision = 1;
    }
    return s_next_connection_revision;
}

esp_err_t cap_mcp_client_set_servers(const cap_mcp_servers_t *servers)
{
    esp_err_t err = cap_mcp_client_validate_servers(servers);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t revisions[CAP_MCP_SERVER_LIMIT] = {0};
    uint32_t version = esp_random();
    portENTER_CRITICAL(&s_server_lock);
    for (size_t i = 0; i < servers->count; ++i) {
        const cap_mcp_server_config_t *entry = &servers->items[i];
        for (size_t j = 0; j < s_servers.count; ++j) {
            const cap_mcp_server_config_t *old = &s_servers.items[j];
            if (entry->id == old->id && strcmp(entry->url, old->url) == 0 &&
                    strcmp(entry->token, old->token) == 0) {
                revisions[i] = s_connection_revisions[j];
                break;
            }
        }
        if (!revisions[i]) {
            revisions[i] = next_connection_revision();
        }
    }
    memcpy(s_connection_revisions, revisions, sizeof(revisions));
    memcpy(s_servers.items, servers->items, sizeof(s_servers.items));
    s_servers.count = servers->count;
    s_servers.version = version;
    s_named_servers = true;
    portEXIT_CRITICAL(&s_server_lock);
    return ESP_OK;
}

void cap_mcp_client_get_servers(cap_mcp_servers_t *servers)
{
    portENTER_CRITICAL(&s_server_lock);
    *servers = s_servers;
    portEXIT_CRITICAL(&s_server_lock);
}

bool cap_mcp_client_get_named_default(char *url, char *token, size_t size)
{
    portENTER_CRITICAL(&s_server_lock);
    bool named = s_named_servers;
    if (named) {
        if (url) {
            strlcpy(url, s_servers.count ? s_servers.items[0].url : "", size);
        }
        if (token) {
            strlcpy(token, s_servers.count ? s_servers.items[0].token : "", size);
        }
    }
    portEXIT_CRITICAL(&s_server_lock);
    return named;
}

esp_err_t cap_mcp_client_set_server(const char *url, const char *bearer_token)
{
    if (!url || (url[0] && cap_mcp_client_validate_url(url) != ESP_OK) ||
            cap_mcp_client_validate_bearer_token(bearer_token) != ESP_OK ||
            (!url[0] && bearer_token[0])) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t version = esp_random();
    portENTER_CRITICAL(&s_server_lock);
    cap_mcp_server_config_t *entry = &s_servers.items[0];
    if (!s_named_servers && (strcmp(entry->url, url) != 0 || strcmp(entry->token, bearer_token) != 0)) {
        entry->id = 1;
        strlcpy(entry->name, "MCP", sizeof(entry->name));
        strlcpy(entry->url, url, sizeof(entry->url));
        strlcpy(entry->token, bearer_token, sizeof(entry->token));
        s_servers.count = url[0] ? 1 : 0;
        s_servers.version = version;
        s_connection_revisions[0] = next_connection_revision();
    }
    portEXIT_CRITICAL(&s_server_lock);
    return ESP_OK;
}

esp_err_t cap_mcp_client_set_server_url(const char *url)
{
    return cap_mcp_client_set_server(url, "");
}

static size_t url_origin_len(const char *url)
{
    const char *host = strstr(url, "://") + 3;
    return (size_t)(host - url) + strcspn(host, "/?");
}

static char *join_url(const char *url, size_t prefix, const char *suffix)
{
    size_t size = prefix + strlen(suffix) + 1;
    if (size > CAP_MCP_URL_SIZE) {
        return NULL;
    }
    char *joined = malloc(size);
    if (joined) {
        memcpy(joined, url, prefix);
        strcpy(joined + prefix, suffix);
    }
    return joined;
}

static esp_err_t resolve_server(cap_mcp_session_t *session, const cJSON *input, const char *test_token,
                                uint32_t test_id, uint32_t expected_revision)
{
    const cJSON *url_item = cJSON_GetObjectItemCaseSensitive(input, "server_url");
    const char *requested_url = cJSON_GetStringValue(url_item);
    const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(input, "server");
    const char *name = cJSON_GetStringValue(name_item);
    const char *endpoint = json_string(input, "endpoint");
    bool custom_endpoint = endpoint && endpoint[0];

    if ((name_item && (!name || !name[0] || url_item)) ||
            (url_item && cap_mcp_client_validate_url(requested_url) != ESP_OK) ||
            (test_token && cap_mcp_client_validate_bearer_token(test_token) != ESP_OK)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* A pending job uses the original connection even if the default or name changes. */
    size_t selected = CAP_MCP_SERVER_LIMIT;
    bool ambiguous = false;
    portENTER_CRITICAL(&s_server_lock);
    for (size_t i = 0; expected_revision != CAP_MCP_DIRECT_REVISION && i < s_servers.count; ++i) {
        const cap_mcp_server_config_t *entry = &s_servers.items[i];
        bool matches = expected_revision ? expected_revision == s_connection_revisions[i] :
                       test_id ? test_id == entry->id :
                       name ? strcasecmp(name, entry->name) == 0 :
                       requested_url ? strcmp(requested_url, entry->url) == 0 : i == 0;
        if (matches) {
            if (selected != CAP_MCP_SERVER_LIMIT) {
                ambiguous = true;
                break;
            }
            selected = i;
        }
    }
    if (selected != CAP_MCP_SERVER_LIMIT && !ambiguous) {
        session->server = s_servers.items[selected];
        session->server_revision = s_connection_revisions[selected];
    }
    portEXIT_CRITICAL(&s_server_lock);

    if (expected_revision && expected_revision != CAP_MCP_DIRECT_REVISION && !session->server_revision) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ambiguous && !test_token && !custom_endpoint) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!session->server_revision) {
        session->server_revision = CAP_MCP_DIRECT_REVISION;
    }
    const char *url = requested_url ? requested_url : session->server.url;
    if (!url[0]) {
        return ESP_ERR_NOT_FOUND;
    }

    const char *token = test_token;
    if (!token && !custom_endpoint && strcmp(url, session->server.url) == 0) {
        token = session->server.token;
    }
    if (token && token[0] && asprintf(&session->authorization, "Bearer %s", token) < 0) {
        return ESP_ERR_NO_MEM;
    }

    size_t origin_len = url_origin_len(url);
    if (custom_endpoint) {
        const char *path = endpoint[0] == '/' ? endpoint + 1 : endpoint;
        if (path[0] == '/' || strstr(path, "://") || strpbrk(path, "#\\ \t\r\n")) {
            return ESP_ERR_INVALID_ARG;
        }
        if (asprintf(&session->url, "%.*s/%s", (int)origin_len, url, path) < 0) {
            return ESP_ERR_NO_MEM;
        }
    } else if (url[origin_len] != '/') {
        if (asprintf(&session->url, "%.*s/" MCP_MDNS_DEFAULT_ENDPOINT "%s",
                     (int)origin_len, url, url + origin_len) < 0) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        session->url = strdup(url);
    }
    return session->url ? cap_mcp_client_validate_url(session->url) : ESP_ERR_NO_MEM;
}

/* HTTP/SSE's POST endpoint is supplied by the server on its GET stream. */
static char *resolve_legacy_endpoint(const char *url, const char *endpoint)
{
    if (!endpoint[0] || strpbrk(endpoint, "#\\ \t\r\n") || strncmp(endpoint, "//", 2) == 0) {
        return NULL;
    }

    size_t origin_len = url_origin_len(url);
    char *resolved;
    if (strstr(endpoint, "://")) {
        if (cap_mcp_client_validate_url(endpoint) != ESP_OK ||
                url_origin_len(endpoint) != origin_len || strncasecmp(url, endpoint, origin_len) != 0) {
            return NULL;
        }
        resolved = strdup(endpoint);
    } else if (endpoint[0] == '/') {
        resolved = join_url(url, origin_len, endpoint);
    } else {
        const char *query = strchr(url + origin_len, '?');
        size_t prefix = query ? (size_t)(query - url) : strlen(url);
        if (endpoint[0] != '?') {
            while (prefix > origin_len && url[prefix - 1] != '/') {
                prefix--;
            }
        }
        resolved = join_url(url, prefix, endpoint);
    }

    if (resolved && cap_mcp_client_validate_url(resolved) == ESP_OK) {
        return resolved;
    }
    free(resolved);
    return NULL;
}

static esp_err_t http_on_header(esp_http_client_event_t *event)
{
    cap_mcp_http_t *http = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_HEADER) {
        return ESP_OK;
    }

    if (strcasecmp(event->header_key, "Content-Type") == 0) {
        const char *mime = "text/event-stream";
        size_t len = strlen(mime);
        if (strncasecmp(event->header_value, mime, len) == 0) {
            const char *suffix = event->header_value + len;
            suffix += strspn(suffix, " \t");
            http->sse = *suffix == '\0' || *suffix == ';';
        }
    } else if (strcasecmp(event->header_key, "Mcp-Session-Id") == 0 && !http->session_id) {
        const char *id = event->header_value;
        size_t len = strlen(id);
        if (!len || len > CAP_MCP_SESSION_ID_MAX) {
            http->header_error = ESP_ERR_INVALID_RESPONSE;
            return ESP_OK;
        }
        for (size_t i = 0; i < len; i++) {
            if ((unsigned char)id[i] < 0x21 || (unsigned char)id[i] > 0x7e) {
                http->header_error = ESP_ERR_INVALID_RESPONSE;
                return ESP_OK;
            }
        }
        http->session_id = strdup(id);
        if (!http->session_id) {
            http->header_error = ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static void http_close(cap_mcp_http_t *http)
{
    if (http->http) {
        esp_http_client_cleanup(http->http);
    }
    free(http->session_id);
}

static esp_err_t http_open(cap_mcp_http_t *http, const cap_mcp_session_t *session,
                           const char *url, esp_http_client_method_t method,
                           const char *body, int timeout_ms, int *status, int64_t *length)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms,
        .buffer_size_tx = CAP_MCP_READ_SIZE,
        .disable_auto_redirect = true,
        .event_handler = http_on_header,
        .user_data = http,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };

    http->http = esp_http_client_init(&config);
    if (!http->http) {
        return ESP_ERR_NO_MEM;
    }

    const char *accept = method == HTTP_METHOD_GET ? "text/event-stream" : "application/json, text/event-stream";
    esp_err_t err = esp_http_client_set_header(http->http, "Accept", accept);
    if (err == ESP_OK && body) {
        err = esp_http_client_set_header(http->http, "Content-Type", "application/json");
    }
    if (err == ESP_OK && session->authorization) {
        err = esp_http_client_set_header(http->http, "Authorization", session->authorization);
    }
    if (err == ESP_OK && session->version && !session->legacy_stream) {
        err = esp_http_client_set_header(http->http, "MCP-Protocol-Version", session->version);
    }
    if (err == ESP_OK && session->session_id) {
        err = esp_http_client_set_header(http->http, "Mcp-Session-Id", session->session_id);
    }
    int remaining = body ? (int)strlen(body) : 0;
    if (err == ESP_OK) {
        err = esp_http_client_open(http->http, remaining);
    }
    if (err != ESP_OK) {
        return err;
    }
    while (remaining > 0) {
        int written = esp_http_client_write(http->http, body, remaining);
        if (written <= 0) {
            return ESP_ERR_HTTP_WRITE_DATA;
        }
        remaining -= written;
        body += written;
    }

    *length = esp_http_client_fetch_headers(http->http);
    *status = esp_http_client_get_status_code(http->http);
    if (*length < 0) {
        return *length == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_ERR_HTTP_FETCH_HEADER;
    }
    if (*status == 401 || *status == 403) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (*status < 200 || *status >= 300) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return http->header_error;
}

static esp_err_t frame_append(cap_mcp_frame_t *frame, const char *data, size_t len)
{
    if (len > CAP_MCP_FRAME_MAX - frame->len) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t needed = frame->len + len + 1;
    if (needed > frame->capacity) {
        size_t capacity = frame->capacity ? frame->capacity * 2 : CAP_MCP_READ_SIZE;
        if (capacity < needed) {
            capacity = needed;
        }
        if (capacity > CAP_MCP_FRAME_MAX + 1) {
            capacity = CAP_MCP_FRAME_MAX + 1;
        }
        char *next = heap_caps_realloc(frame->data, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!next && capacity <= CAP_MCP_INTERNAL_FRAME_MAX) {
            next = heap_caps_realloc(frame->data, capacity, MALLOC_CAP_8BIT);
        }
        if (!next) {
            return ESP_ERR_NO_MEM;
        }
        frame->data = next;
        frame->capacity = capacity;
    }
    memcpy(frame->data + frame->len, data, len);
    frame->len += len;
    frame->data[frame->len] = '\0';
    return ESP_OK;
}

static cJSON *take_rpc_result(cJSON *message, unsigned int id)
{
    const char *version = json_string(message, "jsonrpc");
    const cJSON *response_id = cJSON_GetObjectItemCaseSensitive(message, "id");
    if (!version || strcmp(version, "2.0") != 0 ||
            !cJSON_IsNumber(response_id) || response_id->valuedouble != id) {
        return NULL;
    }
    cJSON *result = cJSON_DetachItemFromObjectCaseSensitive(message, "result");
    if (cJSON_IsObject(result)) {
        return result;
    }
    cJSON_Delete(result);
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(message, "error");
    const char *reason = json_string(error, "message");
    if (!reason) {
        return NULL;
    }
    result = cJSON_CreateObject();
    if (cJSON_AddStringToObject(result, "error_message", reason)) {
        return result;
    }
    cJSON_Delete(result);
    return NULL;
}

cJSON *cap_mcp_parse_json(const char *text, size_t length)
{
    unsigned int depth = 0;
    bool quoted = false;
    for (size_t i = 0; i < length; i++) {
        char ch = text[i];
        if (ch == '\0') {
            return NULL;
        }
        if (quoted && ch == '\\' && i + 1 < length) {
            i++;
        } else if (ch == '"') {
            quoted = !quoted;
        } else if (!quoted && (ch == '[' || ch == '{')) {
            if (++depth > CAP_MCP_JSON_DEPTH_MAX) {
                return NULL;
            }
        } else if (!quoted && (ch == ']' || ch == '}')) {
            if (!depth) {
                return NULL;
            }
            depth--;
        }
    }
    return cJSON_ParseWithLengthOpts(text, length + 1, NULL, true);
}

static cJSON *parse_rpc(const cap_mcp_frame_t *frame, unsigned int id)
{
    cJSON *root = cap_mcp_parse_json(frame->data, frame->len);
    cJSON *result = NULL;
    if (cJSON_IsArray(root)) {
        /* Some 2025-03-26 servers wrap a response in a JSON-RPC batch. */
        cJSON *item;
        cJSON_ArrayForEach(item, root) {
            result = take_rpc_result(item, id);
            if (result) {
                break;
            }
        }
    } else {
        result = take_rpc_result(root, id);
    }
    cJSON_Delete(root);
    return result;
}

static esp_err_t read_sse_chunk(cap_mcp_frame_t *frame, const char *data, size_t len,
                                unsigned int id, cJSON **result, char **endpoint)
{
    while (len > 0) {
        const char *newline = memchr(data, '\n', len);
        size_t count = newline ? (size_t)(newline - data) : len;
        esp_err_t err = frame_append(frame, data, count);
        if (err != ESP_OK || !newline) {
            return err;
        }
        data += count + 1;
        len -= count + 1;
        if (frame->len > frame->line_start && frame->data[frame->len - 1] == '\r') {
            frame->data[--frame->len] = '\0';
        }
        char *line = frame->data + frame->line_start;
        if (!line[0]) {
            if (frame->line_start > 0) {
                frame->data[--frame->len] = '\0'; /* Remove the last data line's newline. */
                if (endpoint && frame->endpoint_event) {
                    *endpoint = strdup(frame->data);
                    return *endpoint ? ESP_OK : ESP_ERR_NO_MEM;
                }
                if (result && (*result = parse_rpc(frame, id))) {
                    return ESP_OK;
                }
            }
            frame->endpoint_event = false;
            frame->line_start = 0;
        } else if (strncmp(line, "data:", 5) == 0) {
            const char *value = line + 5 + (line[5] == ' ');
            size_t value_len = strlen(value);
            memmove(line, value, value_len);
            frame->line_start += value_len + 1;
            frame->data[frame->line_start - 1] = '\n';
        } else if (strncmp(line, "event:", 6) == 0) {
            const char *value = line + 6 + (line[6] == ' ');
            frame->endpoint_event = strcmp(value, "endpoint") == 0;
        }
        frame->len = frame->line_start;
        frame->data[frame->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t read_reply(cap_mcp_http_t *http, int64_t length, unsigned int id,
                            int timeout_ms, cJSON **result, char **endpoint)
{
    if (!http->sse && length > CAP_MCP_FRAME_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (endpoint && !http->sse) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cap_mcp_frame_t frame = {0};
    char *data = malloc(CAP_MCP_READ_SIZE);
    if (!data) {
        return ESP_ERR_NO_MEM;
    }
    /* ESP-IDF read can wait for the whole buffer even after a complete SSE event. */
    if (http->sse) {
        esp_http_client_set_timeout_ms(http->http, CAP_MCP_SSE_READ_MS);
    }
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    esp_err_t err = ESP_OK;
    while (esp_timer_get_time() < deadline) {
        int len = esp_http_client_read(http->http, data, CAP_MCP_READ_SIZE);
        if (len == -ESP_ERR_HTTP_EAGAIN && http->sse) {
            continue;
        }
        if (len < 0) {
            err = len == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (len == 0) {
            break;
        }
        err = http->sse ? read_sse_chunk(&frame, data, len, id, result, endpoint)
                        : frame_append(&frame, data, len);
        if (err != ESP_OK || (result && *result) || (endpoint && *endpoint) ||
                (length > 0 && frame.len == (size_t)length && !http->sse)) {
            break;
        }
    }
    if (err == ESP_OK && !http->sse && frame.len &&
            ((length > 0 && frame.len == (size_t)length) || esp_http_client_is_complete_data_received(http->http))) {
        *result = parse_rpc(&frame, id);
    }
    if (err == ESP_OK && !(result && *result) && !(endpoint && *endpoint)) {
        err = esp_timer_get_time() >= deadline ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_RESPONSE;
    }

    free(frame.data);
    free(data);
    return err;
}

static esp_err_t post_rpc(cap_mcp_session_t *session, const char *body, unsigned int id,
                          int timeout_ms, int *status, cJSON **result)
{
    cap_mcp_http_t post = {0};
    int64_t length;
    const char *url = session->legacy_stream ? session->legacy_post_url : session->url;
    esp_err_t err = http_open(&post, session, url, HTTP_METHOD_POST, body, timeout_ms, status, &length);
    if (err == ESP_OK && result) {
        if (session->legacy_stream) {
            http_close(&post);
            return read_reply(session->legacy_stream, 0, id, timeout_ms, result, NULL);
        }
        err = read_reply(&post, length, id, timeout_ms, result, NULL);
        if (err == ESP_OK && id == 1) {
            session->session_id = post.session_id;
            post.session_id = NULL;
        }
    }
    http_close(&post);
    return err;
}

static esp_err_t legacy_open(cap_mcp_session_t *session)
{
    session->legacy_stream = calloc(1, sizeof(*session->legacy_stream));
    if (!session->legacy_stream) {
        return ESP_ERR_NO_MEM;
    }

    int status;
    int64_t length;
    char *endpoint = NULL;
    esp_err_t err = http_open(session->legacy_stream, session, session->url, HTTP_METHOD_GET,
                              NULL, CAP_MCP_HTTP_TIMEOUT_MS, &status, &length);
    if (err == ESP_OK) {
        err = read_reply(session->legacy_stream, length, 0, CAP_MCP_HTTP_TIMEOUT_MS, NULL, &endpoint);
    }
    if (err == ESP_OK) {
        session->legacy_post_url = resolve_legacy_endpoint(session->url, endpoint);
        if (!session->legacy_post_url) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    free(endpoint);
    return err;
}

static esp_err_t rpc_call(cap_mcp_session_t *session, const char *method, cJSON *params,
                          unsigned int id, int timeout_ms, int *status, cJSON **result)
{
    cJSON *request = cJSON_CreateObject();
    if (!request || !cJSON_AddStringToObject(request, "jsonrpc", "2.0") ||
            !cJSON_AddStringToObject(request, "method", method) ||
            (id && !cJSON_AddNumberToObject(request, "id", id)) ||
            (params && !cJSON_AddItemReferenceToObject(request, "params", params))) {
        cJSON_Delete(request);
        return ESP_ERR_NO_MEM;
    }
    char *body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = post_rpc(session, body, id, timeout_ms, status, result);
    cJSON_free(body);
    return err;
}

static esp_err_t initialize(cap_mcp_session_t *session, const char *version, int *status)
{
    cJSON *params = cJSON_CreateObject();
    cJSON *client = cJSON_AddObjectToObject(params, "clientInfo");
    if (!client || !cJSON_AddStringToObject(client, "name", "esp-claw") ||
            !cJSON_AddStringToObject(client, "version", "1.0.0") ||
            !cJSON_AddStringToObject(params, "protocolVersion", version) ||
            !cJSON_AddObjectToObject(params, "capabilities")) {
        cJSON_Delete(params);
        return ESP_ERR_NO_MEM;
    }
    cJSON *result = NULL;
    esp_err_t err = rpc_call(session, "initialize", params, 1, CAP_MCP_HTTP_TIMEOUT_MS, status, &result);
    cJSON_Delete(params);
    if (err == ESP_OK) {
        static const char *const supported[] = {
            CAP_MCP_STREAMABLE_VERSION, "2025-06-18", "2025-03-26", CAP_MCP_LEGACY_VERSION,
        };
        const char *negotiated = json_string(result, "protocolVersion");
        for (size_t i = 0; negotiated && i < sizeof(supported) / sizeof(supported[0]); i++) {
            if (strcmp(supported[i], negotiated) == 0) {
                session->version = supported[i];
                break;
            }
        }
        err = negotiated ? (session->version ? ESP_OK : ESP_ERR_INVALID_VERSION) : ESP_ERR_INVALID_RESPONSE;
    }
    cJSON_Delete(result);
    return err;
}

static void session_close(cap_mcp_session_t *session)
{
    if (session->legacy_stream) {
        http_close(session->legacy_stream);
    } else if (session->session_id && session->version) {
        cap_mcp_http_t http = {0};
        int status;
        int64_t length;
        (void)http_open(&http, session, session->url, HTTP_METHOD_DELETE, NULL,
                        CAP_MCP_END_TIMEOUT_MS, &status, &length);
        http_close(&http);
    }
    free(session->legacy_stream);
    free(session->legacy_post_url);
    free(session->session_id);
    free(session->authorization);
    free(session->url);
    free(session);
}

static const cJSON *argument_schema(const cJSON *schema, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(schema, "properties"), name);
}

static void fill_image_request_id(cJSON *arguments, const cJSON *schema, const char *name, uint32_t turn_id)
{
    const cJSON *property = argument_schema(schema, name);
    const char *type = json_string(property, "type");
    if (!type || strcmp(type, "string") != 0 ||
            cJSON_GetObjectItemCaseSensitive(property, "pattern") ||
            cJSON_GetObjectItemCaseSensitive(property, "enum") ||
            cJSON_GetObjectItemCaseSensitive(property, "const")) {
        return;
    }
    const cJSON *required;
    cJSON_ArrayForEach(required, cJSON_GetObjectItemCaseSensitive(schema, "required")) {
        if (cJSON_IsString(required) && strcmp(required->valuestring, name) == 0) {
            break;
        }
    }
    if (!required) {
        return;
    }
    char id[37];
    const cJSON *maximum = cJSON_GetObjectItemCaseSensitive(property, "maxLength");
    const cJSON *minimum = cJSON_GetObjectItemCaseSensitive(property, "minLength");
    if ((cJSON_IsNumber(maximum) && maximum->valuedouble < sizeof(id) - 1) ||
            (cJSON_IsNumber(minimum) && minimum->valuedouble > sizeof(id) - 1)) {
        return;
    }

    /* Identical arguments retried in one turn keep the same remote idempotency key. */
    char *serialized = cJSON_PrintUnformatted(arguments);
    if (!serialized) {
        return;
    }
    uint64_t hash = UINT64_C(14695981039346656037) ^ turn_id;
    for (const unsigned char *p = (const unsigned char *)serialized; *p; ++p) {
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    }
    cJSON_free(serialized);
    uint32_t random = esp_random();
    portENTER_CRITICAL(&s_server_lock);
    if (!s_image_id_seed) {
        s_image_id_seed = random ? random : 1;
    }
    uint32_t seed = turn_id ? s_image_id_seed : random;
    portEXIT_CRITICAL(&s_server_lock);
    snprintf(id, sizeof(id), "%08" PRIx32 "-%04" PRIx32 "-4%03" PRIx32 "-8%03" PRIx64 "-%012" PRIx64,
             seed, turn_id >> 16, turn_id & 0xfffU, hash >> 52, hash & UINT64_C(0xffffffffffff));
    cJSON_AddStringToObject(arguments, name, id);
}

static bool compatible_status_tool(const cJSON *tool, const cJSON *schema, const char *id_argument)
{
    const cJSON *annotations = cJSON_GetObjectItemCaseSensitive(tool, "annotations");
    const cJSON *read_only = cJSON_GetObjectItemCaseSensitive(annotations, "readOnlyHint");
    const char *type = json_string(argument_schema(schema, id_argument), "type");
    if (!type || (strcmp(type, "string") != 0 && strcmp(type, "integer") != 0 && strcmp(type, "number") != 0) ||
            cJSON_IsFalse(read_only) || (!cJSON_IsTrue(read_only) &&
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(annotations, "destructiveHint")))) {
        return false;
    }
    const cJSON *required;
    cJSON_ArrayForEach(required, cJSON_GetObjectItemCaseSensitive(schema, "required")) {
        if (!cJSON_IsString(required) || strcmp(required->valuestring, id_argument) != 0) {
            return false;
        }
    }
    return true;
}

static void discover_image_tools(cap_mcp_session_t *session, const cJSON *call_params,
                                 cap_mcp_call_options_t *options)
{
    const cap_mcp_image_delivery_t *delivery = options->delivery;
    const char *call_name = json_string(call_params, "name");
    cJSON *arguments = cJSON_GetObjectItemCaseSensitive(call_params, "arguments");
    bool request_checked = !delivery->request_id_argument ||
                           cJSON_GetObjectItemCaseSensitive(arguments, delivery->request_id_argument);
    bool status_checked = !delivery->status_tool || strcmp(call_name, delivery->status_tool) == 0;
    cJSON *page_params = NULL;
    for (int rpc_id = 3; rpc_id < 7 && !(request_checked && status_checked); ++rpc_id) {
        cJSON *page = NULL;
        int status;
        esp_err_t err = rpc_call(session, "tools/list", page_params, rpc_id,
                                 CAP_MCP_HTTP_TIMEOUT_MS, &status, &page);
        cJSON_Delete(page_params);
        page_params = NULL;
        const cJSON *tool;
        cJSON_ArrayForEach(tool, cJSON_GetObjectItemCaseSensitive(page, "tools")) {
            const char *name = json_string(tool, "name");
            const cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
            if (!request_checked && name && strcmp(name, call_name) == 0) {
                request_checked = true;
                fill_image_request_id(arguments, schema, delivery->request_id_argument, options->turn_id);
            }
            if (!status_checked && name && strcmp(name, delivery->status_tool) == 0) {
                status_checked = true;
                options->can_poll_image = compatible_status_tool(tool, schema, delivery->id_argument);
            }
        }
        const char *cursor = json_string(page, "nextCursor");
        if (err == ESP_OK && cursor && cursor[0] && !(request_checked && status_checked)) {
            page_params = cJSON_CreateObject();
            if (!cJSON_AddStringToObject(page_params, "cursor", cursor)) {
                cJSON_Delete(page_params);
                page_params = NULL;
            }
        }
        cJSON_Delete(page);
        if (!page_params) {
            break;
        }
    }
    cJSON_Delete(page_params);
}

static void set_catalog_page_size(const cJSON *schema, cJSON *arguments)
{
    const cJSON *limit = argument_schema(schema, "limit");
    const char *type = json_string(limit, "type");
    if (!type || (strcmp(type, "integer") != 0 && strcmp(type, "number") != 0)) {
        return;
    }
    int size = CAP_MCP_CATALOG_PAGE_SIZE;
    const cJSON *minimum = cJSON_GetObjectItemCaseSensitive(limit, "minimum");
    const cJSON *maximum = cJSON_GetObjectItemCaseSensitive(limit, "maximum");
    if (cJSON_IsNumber(maximum) && maximum->valueint < size) {
        size = maximum->valueint;
    }
    if (cJSON_IsNumber(minimum) && minimum->valueint > size) {
        size = minimum->valueint;
    }
    if (size > 0 && (!cJSON_IsNumber(maximum) || size <= maximum->valueint)) {
        cJSON_AddNumberToObject(arguments, "limit", size);
    }
}

static bool prepare_model_catalog(cap_mcp_session_t *session, cJSON *arguments, bool image_only, int rpc_id)
{
    cJSON *page = NULL;
    int status;
    bool available = false;
    if (rpc_call(session, "tools/list", NULL, rpc_id, CAP_MCP_HTTP_TIMEOUT_MS, &status, &page) == ESP_OK) {
        const cJSON *tool;
        cJSON_ArrayForEach(tool, cJSON_GetObjectItemCaseSensitive(page, "tools")) {
            const char *name = json_string(tool, "name");
            if (!name || strcmp(name, "list_models") != 0) {
                continue;
            }
            const cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
            set_catalog_page_size(schema, arguments);
            if (image_only && argument_schema(schema, "kind")) {
                cJSON_AddStringToObject(arguments, "kind", "image");
            }
            available = true;
            const cJSON *required;
            cJSON_ArrayForEach(required, cJSON_GetObjectItemCaseSensitive(schema, "required")) {
                const char *key = cJSON_GetStringValue(required);
                if (!key || !cJSON_GetObjectItemCaseSensitive(arguments, key)) {
                    available = false;
                    break;
                }
            }
            break;
        }
    }
    cJSON_Delete(page);
    return available;
}

static const cJSON *catalog_items(const cJSON *page)
{
    if (cJSON_IsArray(page)) {
        return page;
    }
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(page, "items");
    if (!cJSON_IsArray(items)) {
        items = cJSON_GetObjectItemCaseSensitive(page, "models");
    }
    if (!cJSON_IsArray(items)) {
        items = cJSON_GetObjectItemCaseSensitive(page, "data");
    }
    return cJSON_IsArray(items) ? items : NULL;
}

static cJSON *catalog_model_ids(const cJSON *result)
{
    const cJSON *items = catalog_items(cJSON_GetObjectItemCaseSensitive(result, "structuredContent"));
    cJSON *decoded = NULL;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(result, "content")) {
        if (items) {
            break;
        }
        const char *text = json_string(entry, "text");
        if (text) {
            cJSON_Delete(decoded);
            decoded = cap_mcp_parse_json(text, strlen(text));
            items = catalog_items(decoded);
        }
    }

    cJSON *ids = cJSON_CreateArray();
    if (ids) {
        cJSON_ArrayForEach(entry, items) {
            if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(entry, "ready"))) {
                continue;
            }
            const char *kind = json_string(entry, "kind");
            if (kind && strcmp(kind, "image") != 0) {
                continue;
            }
            const char *id = cJSON_GetStringValue(entry);
            if (!id) id = json_string(entry, "key");
            if (!id) id = json_string(entry, "id");
            if (!id) id = json_string(entry, "model");
            if (id && id[0] && strlen(id) <= MCP_IMAGE_FIELD_MAX && !strpbrk(id, "\r\n\t")) {
                cJSON *value = cJSON_CreateString(id);
                if (!value || !cJSON_AddItemToArray(ids, value)) {
                    cJSON_Delete(value);
                    break;
                }
                if (cJSON_GetArraySize(ids) >= CAP_MCP_CATALOG_PAGE_SIZE) {
                    break;
                }
            }
        }
    }
    cJSON_Delete(decoded);
    return ids;
}

/* A rejected generation can be corrected with IDs from this same MCP server. */
static void add_image_catalog_hint(cap_mcp_session_t *session, cJSON *result)
{
    cJSON *params = cJSON_CreateObject();
    cJSON *arguments = cJSON_AddObjectToObject(params, "arguments");
    cJSON *page = NULL;
    int status;
    if (arguments && cJSON_AddStringToObject(params, "name", "list_models") &&
            prepare_model_catalog(session, arguments, true, 8) &&
            rpc_call(session, "tools/call", params, 9, CAP_MCP_HTTP_TIMEOUT_MS, &status, &page) == ESP_OK &&
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(page, "isError"))) {
        cJSON *ids = catalog_model_ids(page);
        if (cJSON_GetArraySize(ids) == 0 || !cJSON_AddItemToObjectCS(result, "esp_claw_model_ids", ids)) {
            cJSON_Delete(ids);
        }
    }
    cJSON_Delete(page);
    cJSON_Delete(params);
}

static esp_err_t request(const cJSON *input, const char *test_token, uint32_t test_id, const char *method,
                         cJSON *params, int timeout_ms, cap_mcp_call_options_t *options, cJSON **result)
{
    int status = 0;
    cap_mcp_session_t *session = calloc(1, sizeof(*session));
    if (!session) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = resolve_server(session, input, test_token, test_id, options ? options->server_revision : 0);
    if (err == ESP_OK && options) {
        options->server_revision = session->server_revision;
    }
    if (err == ESP_OK) {
        err = initialize(session, CAP_MCP_STREAMABLE_VERSION, &status);
    }
    /* Only fall back after an explicit HTTP rejection of initialize. Never retry tools/call. */
    if (err == ESP_ERR_INVALID_RESPONSE &&
            (status == 400 || status == 404 || status == 405 || status == 406 || status == 415)) {
        err = legacy_open(session);
        if (err == ESP_OK) {
            err = initialize(session, CAP_MCP_LEGACY_VERSION, &status);
        }
    }
    if (err == ESP_OK) {
        err = rpc_call(session, "notifications/initialized", NULL, 0, CAP_MCP_HTTP_TIMEOUT_MS, &status, NULL);
    }
    if (err == ESP_OK) {
        if (options && options->delivery) {
            discover_image_tools(session, params, options);
        }
        const char *name = json_string(params, "name");
        cJSON *arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");
        if (name && strcmp(name, "list_models") == 0 && !cJSON_HasObjectItem(arguments, "limit")) {
            prepare_model_catalog(session, arguments, false, 3);
        }
        err = rpc_call(session, method, params, 2, timeout_ms, &status, result);
        if (err == ESP_OK && result && name && strcmp(name, "generate_image") == 0 &&
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(*result, "isError"))) {
            add_image_catalog_hint(session, *result);
        }
    }
    session_close(session);
    return err;
}

static esp_err_t list_tools(const cJSON *input, const char *test_token, uint32_t test_id, cJSON **result)
{
    if (!result) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = NULL;
    if (!cJSON_IsObject(input)) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *params = NULL;
    const char *cursor = json_string(input, "cursor");
    if (cursor) {
        params = cJSON_CreateObject();
        if (!cJSON_AddStringToObject(params, "cursor", cursor)) {
            cJSON_Delete(params);
            return ESP_ERR_NO_MEM;
        }
    }
    esp_err_t err = request(input, test_token, test_id, "tools/list", params, CAP_MCP_HTTP_TIMEOUT_MS, NULL, result);
    cJSON_Delete(params);
    return err;
}

esp_err_t cap_mcp_client_list_tools(const cJSON *input, cJSON **result)
{
    return list_tools(input, NULL, 0, result);
}

esp_err_t cap_mcp_client_test_server(const char *url, const char *bearer_token, cJSON **result)
{
    return cap_mcp_client_test_server_entry(url, bearer_token, 0, result);
}

esp_err_t cap_mcp_client_test_server_entry(const char *url, const char *bearer_token,
                                           uint32_t server_id, cJSON **result)
{
    if (!result) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = NULL;
    if (cap_mcp_client_validate_url(url) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *input = cJSON_CreateObject();
    if (!cJSON_AddStringToObject(input, "server_url", url)) {
        cJSON_Delete(input);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = list_tools(input, bearer_token, server_id, result);
    cJSON_Delete(input);
    return err;
}

esp_err_t cap_mcp_call_remote_tool(const cJSON *input, cap_mcp_call_options_t *options, cJSON **result)
{
    if (!result) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = NULL;
    const char *name = json_string(input, "tool_name");
    if (!name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(input, "arguments");
    const char *request_id = options && options->delivery ? options->delivery->request_id_argument : NULL;
    bool fill_id = request_id && !cJSON_GetObjectItemCaseSensitive(arguments, request_id);
    bool paginate = strcmp(name, "list_models") == 0 && !cJSON_HasObjectItem(arguments, "limit");
    bool copy_arguments = fill_id || paginate;
    cJSON *owned_args = cJSON_IsObject(arguments) && copy_arguments ? cJSON_Duplicate(arguments, true) : NULL;
    cJSON *params = cJSON_CreateObject();
    if (!cJSON_AddStringToObject(params, "name", name) ||
            !(cJSON_IsObject(arguments) ?
              (copy_arguments ? cJSON_AddItemToObjectCS(params, "arguments", owned_args) :
                         cJSON_AddItemReferenceToObject(params, "arguments", (cJSON *)arguments)) :
              cJSON_AddObjectToObject(params, "arguments") != NULL)) {
        cJSON_Delete(owned_args);
        cJSON_Delete(params);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = request(input, NULL, 0, "tools/call", params, CAP_MCP_TOOL_TIMEOUT_MS, options, result);
    cJSON_Delete(params);
    return err;
}
