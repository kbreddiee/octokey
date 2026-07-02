/* espkvm dongle — ST7735 status LCD (LILYGO T-Dongle-S3).
 * Shows this dongle's slot number big and bright, plus ACTIVE/idle and
 * link state, so you can tell your machines apart from across the desk. */
/* SPDX-License-Identifier: MIT */
#pragma once

#include "esp_err.h"

/* Initialises the panel and starts the refresh task. Failure is non-fatal
 * (a dongle without a working LCD still forwards input). */
esp_err_t lcd_init(void);
