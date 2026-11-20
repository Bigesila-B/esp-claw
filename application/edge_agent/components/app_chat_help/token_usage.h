/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

typedef struct {
    uint64_t tokens;
    uint64_t reported;
    uint64_t unreported;
    uint64_t cache_input;
    uint64_t cache_hit;
} app_token_period_t;

typedef struct {
    bool available;
    bool durable;
    app_token_period_t today;
    app_token_period_t month;
} app_token_snapshot_t;

esp_err_t app_token_usage_init(void);
void app_token_usage_snapshot(const struct tm *date, app_token_snapshot_t *out);
