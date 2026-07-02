/* espkvm hub — USB host: HID keyboard/mouse/receiver handling. */
/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "kvm_hidparse.h"

/* Called from the HID host task for every decoded input report.
 * Implementations must only queue, never block. */
typedef void (*usb_input_cb_t)(const kvm_hidp_out_t *ev);

esp_err_t usb_kbd_init(usb_input_cb_t cb);

/* True while at least one HID interface is open (for the OLED status). */
bool usb_kbd_connected(void);
