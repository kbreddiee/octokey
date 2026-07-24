/* espkvm dongle — TinyUSB composite HID device (keyboard + mouse + consumer). */
/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t usb_dev_init(void);

void usb_dev_kbd(uint8_t mods, const uint8_t keys[6]);
void usb_dev_mouse(uint8_t buttons, int16_t dx, int16_t dy,
                   int8_t wheel, int8_t pan);
void usb_dev_consumer(uint16_t usage);

/* Zero out keyboard, mouse buttons and consumer state on the host. */
void usb_dev_release_all(void);

/* True if any key / button / media usage is currently reported as held —
 * drives the stuck-key watchdog when the radio link drops. */
bool usb_dev_anything_held(void);

bool usb_dev_mounted(void);
