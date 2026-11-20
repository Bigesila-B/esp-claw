/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

/* Initialize after NVS and the device timezone are configured. */
esp_err_t app_chat_help_init(void);
void app_chat_help_set_auto_sticker_rate(unsigned rate);

/* Register after the router and its outbound bindings are ready. */
esp_err_t app_chat_help_register(void);

/* The caller owns the returned snapshot. */
cJSON *app_chat_help_json(void);

/* Identify help replies in the existing outbound chat protocol. */
bool app_chat_help_is_reply(const char *text);
