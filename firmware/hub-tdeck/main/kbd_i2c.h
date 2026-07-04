/*
 * espkvm hub-tdeck — I2C keyboard driver + hotkey chord.
 *
 * The physical keyboard has NO Ctrl/Alt/Win/arrows/Esc/Tab/F-keys (see
 * README/BUILD.md) — only letters, Shift x2, Space, Enter, Backspace, a
 * Symbol layer key, and a Mic key we don't use. To make it useful:
 *
 *   physical ALT key  -> sent to the target as HID Left-Ctrl
 *   double-tap ALT, then a digit  -> switch slots (same trick as the
 *                                    double-Right-Ctrl chord on the
 *                                    other hub variants, see input.c)
 *   physical Symbol key -> local layer-select only (never forwarded);
 *                          held while rolling the trackball -> arrow
 *                          keys instead of mouse movement (trackball.c)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

esp_err_t kbd_i2c_init(void);

/* True while the physical Symbol key is currently held — trackball.c
 * reads this to decide mouse-move vs. arrow-key mode. */
bool kbd_i2c_symbol_held(void);

/* The board-wide I2C bus (keyboard + GT911 touch share it). Created by
 * kbd_i2c_init even if the keyboard itself fails to probe; NULL only if
 * the bus itself couldn't be brought up. touch.c attaches through this. */
i2c_master_bus_handle_t kbd_i2c_bus(void);

/* HID modifier byte currently held on the physical keyboard (Ctrl from
 * the remapped ALT, Shift) — merged into soft-key taps by touch.c so
 * e.g. physical-Shift + on-screen-Tab sends a real Shift+Tab. */
uint8_t kbd_i2c_mods(void);
