/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"

#include "cap_mcp_client.h"
#include "cap_mcp_client_priv.h"

static esp_err_t cap_mcp_client_group_init(void)
{
    return ESP_OK;
}

static esp_err_t cap_mcp_format_request_error(esp_err_t err, char *output, size_t output_size)
{
    if (err == ESP_ERR_NOT_FOUND) {
        snprintf(output, output_size, "Error: MCP server not found. Use mcp_list_servers to find saved names or configure a server in Web settings.");
    } else if (err == ESP_ERR_NOT_ALLOWED) {
        snprintf(output, output_size, "Error: The selected MCP server rejected authentication. Check its Bearer token in Web settings.");
    } else if (err == ESP_ERR_INVALID_SIZE) {
        snprintf(output, output_size, "Error: MCP response too large (ESP_ERR_INVALID_SIZE). "
                 "For list_models, use a smaller page or the MCP model directory script.");
    } else {
        snprintf(output, output_size, "Error: MCP request failed (%s)", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t cap_mcp_servers_execute(const char *input, const claw_cap_call_context_t *ctx,
                                         char *output, size_t size)
{
    (void)input;
    (void)ctx;
    cap_mcp_servers_t *servers = malloc(sizeof(*servers));
    if (!servers) {
        return ESP_ERR_NO_MEM;
    }
    cap_mcp_client_get_servers(servers);
    size_t offset = 0;
    for (size_t i = 0; i < servers->count && offset < size; ++i) {
        int written = snprintf(output + offset, size - offset, "- %s%s\n", servers->items[i].name,
                               i == 0 ? " (default)" : "");
        if (written < 0 || (size_t)written >= size - offset) {
            break;
        }
        offset += (size_t)written;
    }
    if (!servers->count) {
        snprintf(output, size, "No saved MCP servers. Open MCP Settings to add one.");
    }
    free(servers);
    return ESP_OK;
}

static bool delivery_string(const cJSON *object, const char *key, const char **value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!item) {
        return true;
    }
    const char *text = cJSON_GetStringValue(item);
    if (!text || !text[0] || strlen(text) > MCP_IMAGE_FIELD_MAX || strpbrk(text, "\r\n\t")) {
        return false;
    }
    *value = text;
    return true;
}

/* Accept flat parameters from model providers that omit the arguments wrapper. */
static esp_err_t normalize_tool_arguments(cJSON *input)
{
    if (!cJSON_IsObject(input)) {
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(input, "arguments");
    if (arguments) {
        return cJSON_IsObject(arguments) ? ESP_OK : ESP_ERR_INVALID_ARG;
    }

    cJSON *args = cJSON_CreateObject();
    if (!args) {
        return ESP_ERR_NO_MEM;
    }
    const cJSON *field;
    cJSON_ArrayForEach(field, input) {
        const char *key = field->string;
        if (strcmp(key, "tool_name") == 0 || strcmp(key, "server") == 0 ||
                strcmp(key, "server_url") == 0 || strcmp(key, "endpoint") == 0 ||
                strcmp(key, "image_delivery") == 0 || strcmp(key, "bearer_token") == 0) {
            continue;
        }
        if (!cJSON_AddItemReferenceToObject(args, key, (cJSON *)field)) {
            cJSON_Delete(args);
            return ESP_ERR_NO_MEM;
        }
    }
    if (!cJSON_AddItemToObjectCS(input, "arguments", args)) {
        cJSON_Delete(args);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void append_image_model_hint(const cJSON *result, char *output, size_t size)
{
    size_t used = strnlen(output, size);
    if (used >= size) {
        return;
    }
    int written = snprintf(output + used, size - used, "\nFor generate_image, copy the exact model ID from list_models(kind=image).");
    if (written < 0 || (size_t)written >= size - used) {
        return;
    }
    used += (size_t)written;
    const cJSON *id;
    cJSON_ArrayForEach(id, cJSON_GetObjectItemCaseSensitive(result, "esp_claw_model_ids")) {
        const char *value = cJSON_GetStringValue(id);
        if (!value || !value[0] || strlen(value) > MCP_IMAGE_FIELD_MAX || strpbrk(value, "\r\n\t")) {
            continue;
        }
        written = snprintf(output + used, size - used, "\n- model: %s", value);
        if (written < 0 || (size_t)written >= size - used) {
            break;
        }
        used += (size_t)written;
    }
}

static esp_err_t cap_mcp_call_execute(const char *input_json,
                                      const claw_cap_call_context_t *ctx,
                                      char *output,
                                      size_t output_size)
{
    cJSON *result = NULL;
    esp_err_t err;

    cJSON *input = cJSON_Parse(input_json);
    err = normalize_tool_arguments(input);
    if (err != ESP_OK) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: MCP tool arguments must be a JSON object.");
        return err;
    }
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(input, "tool_name"));
    const cJSON *image = cJSON_GetObjectItemCaseSensitive(input, "image_delivery");
    bool common_image_tool = name && strcmp(name, "generate_image") == 0;
    cap_mcp_image_delivery_t delivery = {
        .fields = { .id = "job_id", .status = "status" },
        .status_tool = common_image_tool ? "get_job" : NULL,
        .id_argument = "job_id",
        .request_id_argument = common_image_tool ? "request_id" : NULL,
    };
    if (image && (!cJSON_IsObject(image) || !delivery_string(image, "status_tool", &delivery.status_tool) ||
                  !delivery_string(image, "id_field", &delivery.fields.id) ||
                  !delivery_string(image, "id_argument", &delivery.id_argument) ||
                  !delivery_string(image, "status_field", &delivery.fields.status) ||
                  !delivery_string(image, "image_field", &delivery.fields.images) ||
                  !delivery_string(image, "request_id_argument", &delivery.request_id_argument))) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Invalid image_delivery options.");
        return ESP_ERR_INVALID_ARG;
    }
    cap_mcp_call_options_t options = {
        .turn_id = ctx ? ctx->request_id : 0,
        .delivery = image || common_image_tool ? &delivery : NULL,
    };
    err = cap_mcp_call_remote_tool(input, options.delivery ? &options : NULL, &result);
    if (err != ESP_OK) {
        cJSON_Delete(input);
        return cap_mcp_format_request_error(err, output, output_size);
    }

    bool failed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")) ||
                  cJSON_IsString(cJSON_GetObjectItemCaseSensitive(result, "error_message"));
    if (failed || !options.delivery || !cap_mcp_deliver_image(input, result, &options, ctx, output, output_size)) {
        cap_mcp_format_result(result, ctx, output, output_size);
    }
    if (failed && common_image_tool) {
        append_image_model_hint(result, output, output_size);
    }
    cJSON_Delete(result);
    cJSON_Delete(input);
    return failed ? ESP_ERR_INVALID_RESPONSE : ESP_OK;
}

static esp_err_t cap_mcp_last_image_execute(const char *input_json, const claw_cap_call_context_t *ctx,
                                            char *output, size_t size)
{
    (void)input_json;
    bool use_target = ctx && ctx->target_channel && ctx->target_channel[0];
    esp_err_t err = ctx ? cap_mcp_client_resend_image(use_target ? ctx->target_channel : ctx->channel,
                                                     use_target ? ctx->target_chat_id : ctx->chat_id) :
                    ESP_ERR_INVALID_ARG;
    snprintf(output, size, "%s",
             err == ESP_OK ? "The latest MCP image was sent to this chat." :
             err == ESP_ERR_NOT_FINISHED ? "This chat's MCP image is still being generated." :
             err == ESP_ERR_NOT_FOUND ? "No recent MCP image job was recorded for this chat." :
             "The latest MCP image could not be retrieved or sent. Check the MCP service.");
    return err;
}

static esp_err_t cap_mcp_list_execute(const char *input_json,
                                      const claw_cap_call_context_t *ctx,
                                      char *output,
                                      size_t output_size)
{
    cJSON *result = NULL;
    const char *error_message = NULL;
    cJSON *tools_array = NULL;
    cJSON *tool = NULL;
    size_t offset = 0;
    esp_err_t err;

    (void)ctx;

    cJSON *input = cJSON_Parse(input_json);
    const char *selected_name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(input, "tool_name"));
    err = cap_mcp_client_list_tools(input, &result);
    if (err != ESP_OK) {
        cJSON_Delete(input);
        return cap_mcp_format_request_error(err, output, output_size);
    }

    error_message = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error_message"));
    if (error_message && error_message[0]) {
        snprintf(output, output_size, "Error: %s", error_message);
        cJSON_Delete(result);
        cJSON_Delete(input);
        return ESP_OK;
    }

    tools_array = cJSON_GetObjectItem(result, "tools");
    if (!cJSON_IsArray(tools_array)) {
        cJSON_Delete(result);
        cJSON_Delete(input);
        return cap_mcp_format_request_error(ESP_ERR_INVALID_RESPONSE, output, output_size);
    }
    if (output_size > 0) {
        cJSON_ArrayForEach(tool, tools_array) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "name"));
            const char *description = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "description"));
            if (selected_name && (!name || strcmp(name, selected_name) != 0)) {
                continue;
            }
            if (selected_name) {
                const cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
                char *schema_json = cJSON_PrintUnformatted(schema);
                int written = snprintf(output, output_size, "%s: %s\nInput schema: %s",
                                       name, description ? description : "", schema_json ? schema_json : "{}");
                cJSON_free(schema_json);
                const cJSON *output_schema = cJSON_GetObjectItemCaseSensitive(tool, "outputSchema");
                if (written >= 0 && (size_t)written < output_size && cJSON_IsObject(output_schema)) {
                    char *output_schema_json = cJSON_PrintUnformatted(output_schema);
                    if (output_schema_json) {
                        snprintf(output + written, output_size - written, "\nOutput schema: %s", output_schema_json);
                        cJSON_free(output_schema_json);
                    }
                }
                cJSON_Delete(result);
                cJSON_Delete(input);
                return ESP_OK;
            }
            int written = snprintf(output + offset,
                                   output_size - offset,
                                   "- %s: %s\n",
                                   name ? name : "(no name)",
                                   description ? description : "");

            if (written < 0 || (size_t)written >= output_size - offset) {
                offset = output_size - 1;
                break;
            }
            offset += (size_t)written;
        }
    }

    cJSON *next_cursor = cJSON_GetObjectItem(result, "nextCursor");
    if (cJSON_IsString(next_cursor) && next_cursor->valuestring[0] && offset < output_size - 1) {
        offset += snprintf(output + offset,
                           output_size - offset,
                           "\n(nextCursor: %s)",
                           next_cursor->valuestring);
    }
    if (offset == 0) {
        snprintf(output, output_size, "%s", selected_name ? "(tool not found on this page)" : "(no tools)");
    } else if (offset >= output_size) {
        output[output_size - 1] = '\0';
    }

    cJSON_Delete(result);
    cJSON_Delete(input);
    return ESP_OK;
}

static esp_err_t cap_mcp_discover_execute(const char *input_json,
                                          const claw_cap_call_context_t *ctx,
                                          char *output,
                                          size_t output_size)
{
    cJSON *root = NULL;
    cJSON *devices = NULL;
    cJSON *device = NULL;
    size_t offset = 0;
    size_t found = 0;
    esp_err_t err;

    (void)ctx;

    err = cap_mcp_discover_services(input_json, &root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: mDNS MCP discovery failed (%s)", esp_err_to_name(err));
        return err;
    }

    devices = cJSON_GetObjectItem(root, "devices");
    if (cJSON_IsArray(devices)) {
        cJSON_ArrayForEach(device, devices) {
            const char *instance = cJSON_GetStringValue(cJSON_GetObjectItem(device, "instance"));
            const char *hostname = cJSON_GetStringValue(cJSON_GetObjectItem(device, "hostname"));
            const char *ip = cJSON_GetStringValue(cJSON_GetObjectItem(device, "ip"));
            const char *endpoint = cJSON_GetStringValue(cJSON_GetObjectItem(device, "endpoint"));
            const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(device, "url"));
            cJSON *port = cJSON_GetObjectItem(device, "port");
            int written = snprintf(output + offset,
                                   output_size - offset,
                                   "instance=%s\nhostname=%s\nip=%s\nport=%u\nendpoint=%s\nurl=%s\n\n",
                                   instance ? instance : "(unknown)",
                                   hostname ? hostname : "(unknown)",
                                   ip ? ip : "(unresolved)",
                                   cJSON_IsNumber(port) ? (unsigned)port->valueint : 0,
                                   endpoint ? endpoint : MCP_MDNS_DEFAULT_ENDPOINT,
                                   url ? url : "(unknown)");

            if (written < 0 || (size_t)written >= output_size - offset) {
                offset = output_size - 1;
                break;
            }
            offset += (size_t)written;
            found++;
        }
    }

    cJSON_Delete(root);
    if (found == 0) {
        snprintf(output, output_size, "(no mcp servers discovered)");
    } else if (offset >= 2) {
        output[offset - 1] = '\0';
    }

    return ESP_OK;
}

static const claw_cap_descriptor_t s_mcp_client_descriptors[] = {
    {
        .id = "mcp_list_servers",
        .name = "mcp_list_servers",
        .family = "mcp",
        .description = "List the saved MCP server names and default. Use a returned name as the server argument for mcp_list_tools and mcp_call_tool.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_mcp_servers_execute,
    },
    {
        .id = "mcp_list_tools",
        .name = "mcp_list_tools",
        .family = "mcp",
        .description = "List tools on the named or default MCP server. Set tool_name to inspect a tool's declared schemas.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"server\":{\"type\":\"string\",\"description\":\"Saved server name\"},\"server_url\":{\"type\":\"string\"},\"endpoint\":{\"type\":\"string\"},\"cursor\":{\"type\":\"string\"},\"tool_name\":{\"type\":\"string\"}}}",
        .execute = cap_mcp_list_execute,
    },
    {
        .id = "mcp_call_tool",
        .name = "mcp_call_tool",
        .family = "mcp",
        .description = "Call a tool on the named or default MCP server. Put remote parameters inside arguments. Use tool schemas to verify parameters. Before generate_image, obtain the exact model ID from list_models if the user gave a display name. Use filters or small pages when declared. For requested images set image_delivery to {} for a direct result, or map the server's read-only status tool and result fields for background delivery.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"server\":{\"type\":\"string\",\"description\":\"Saved server name\"},"
        "\"server_url\":{\"type\":\"string\"},\"endpoint\":{\"type\":\"string\"},\"tool_name\":{\"type\":\"string\"},"
        "\"arguments\":{\"type\":\"object\"},\"image_delivery\":{\"type\":\"object\",\"properties\":{"
        "\"status_tool\":{\"type\":\"string\",\"description\":\"Read-only tool that checks the job\"},"
        "\"id_field\":{\"type\":\"string\",\"description\":\"Dot path to the returned job ID, default job_id\"},"
        "\"id_argument\":{\"type\":\"string\",\"description\":\"Job ID argument of the status tool, default job_id\"},"
        "\"status_field\":{\"type\":\"string\",\"description\":\"Dot path to the job state, default status\"},"
        "\"image_field\":{\"type\":\"string\",\"description\":\"Optional dot path to the result image URL or URL array\"},"
        "\"request_id_argument\":{\"type\":\"string\",\"description\":\"Optional required string argument for a new idempotency key\"}"
        "}}},\"required\":[\"tool_name\"]}",
        .execute = cap_mcp_call_execute,
    },
    {
        .id = "mcp_last_image",
        .name = "mcp_last_image",
        .family = "mcp",
        .description = "Resend the latest MCP-generated image in this same chat. Use when the user asks to see or resend the image they just generated.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_mcp_last_image_execute,
    },
    {
        .id = "mcp_discover",
        .name = "mcp_discover",
        .family = "mcp",
        .description = "Discover MCP servers advertised on the local network.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"timeout_ms\":{\"type\":\"integer\"},\"include_self\":{\"type\":\"boolean\"}}}",
        .execute = cap_mcp_discover_execute,
    },
};

static const claw_cap_group_t s_mcp_client_group = {
    .group_id = "cap_mcp_client",
    .descriptors = s_mcp_client_descriptors,
    .descriptor_count = sizeof(s_mcp_client_descriptors) / sizeof(s_mcp_client_descriptors[0]),
    .group_init = cap_mcp_client_group_init,
};

esp_err_t cap_mcp_client_register_group(void)
{
    if (claw_cap_group_exists(s_mcp_client_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_mcp_client_group);
}
