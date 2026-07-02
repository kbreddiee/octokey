/* espkvm hub — minimal SSD1306 128x64 I2C driver + 5x7 font. */
/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define OLED_W 128
#define OLED_H 64

esp_err_t oled_init(void);

void oled_clear(void);
void oled_pixel(int x, int y, bool on);
void oled_fill_rect(int x, int y, int w, int h, bool on);

/* 6x8-pixel character cells: col 0..20, row 0..7. `inv` = inverse video. */
void oled_text(uint8_t col, uint8_t row, const char *s, bool inv);

/* Scaled text at arbitrary pixel position (scale 2 => 12x16 cells...). */
void oled_text_scaled(int x, int y, const char *s, uint8_t scale);

/* Push the framebuffer to the panel. */
void oled_flush(void);
