/* espkvm — shared 5x7 bitmap font (ASCII 0x20..0x7E).
 * Column-major, 5 bytes per glyph, LSB = top row.
 * Used by the hub's SSD1306 OLED and the dongle's ST7735 LCD. */
/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_FONT_FIRST 0x20
#define KVM_FONT_LAST  0x7E

extern const uint8_t kvm_font5x7[95][5];

/* Glyph for a character, falling back to space for out-of-range input. */
static inline const uint8_t *kvm_font_glyph(char c)
{
    unsigned char u = (unsigned char)c;
    if (u < KVM_FONT_FIRST || u > KVM_FONT_LAST) {
        u = KVM_FONT_FIRST;
    }
    return kvm_font5x7[u - KVM_FONT_FIRST];
}

#ifdef __cplusplus
}
#endif
