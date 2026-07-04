/*
 * espkvm hub-tdeck — GT911 touch panel as a trackpad.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_err.h"

/* Requires kbd_i2c_init() to have run first (shares its I2C bus). */
esp_err_t touch_init(void);
