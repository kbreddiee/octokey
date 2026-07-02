/* espkvm hub — input pipeline: hotkey chord interception + forwarding. */
/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "kvm_hidparse.h"

typedef enum {
    INPUT_EVT_CMD_ON,        /* double Right-Ctrl detected, awaiting digit */
    INPUT_EVT_CMD_OFF,       /* command mode left (timeout/Esc/success)    */
    INPUT_EVT_CMD_SWITCH,    /* digit selected a paired slot (arg = slot)  */
    INPUT_EVT_CMD_INVALID,   /* digit selected an empty slot (arg = slot)  */
} input_event_t;

typedef void (*input_event_cb_t)(input_event_t evt, uint8_t arg);

esp_err_t input_init(input_event_cb_t cb);

/* Entry point for decoded USB HID events (called from the HID host task;
 * copies into the input queue and returns immediately). */
void input_submit(const kvm_hidp_out_t *ev);
