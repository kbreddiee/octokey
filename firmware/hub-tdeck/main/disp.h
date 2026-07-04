/* espkvm hub-tdeck — ST7789 320x240 status screen. */
/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t disp_init(void);

/* Redraws the whole screen from current link/store state. Cheap enough
 * to call on every relevant event (slot switch, health change, toast). */
void disp_update(void);

/* Transient one-line banner shown for a couple of seconds (pairing
 * results, forget confirmation, etc.), then the normal screen returns. */
void disp_toast(const char *text);

/* Persistent "PAIRING... Ns left" screen; NULL/false to leave it. */
void disp_set_pairing(bool active, int seconds_left);

/* Soft-key bar hit test (screen coordinates). Returns true and sets
 * *usage when (x,y) lands on one of the on-screen keys — touch.c calls
 * this to route taps to keystrokes instead of mouse clicks. Usages
 * 0xE0..0xE7 mean "this key is a HID modifier" (e.g. 0xE3 = left Win).
 * Always false while the pairing screen is up (no keys drawn then). */
bool disp_softkey_hit(int x, int y, uint8_t *usage);
