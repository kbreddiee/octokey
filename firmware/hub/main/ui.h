/* espkvm hub — encoder + OLED user interface. */
/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "link.h"
#include "input.h"

esp_err_t ui_init(void);

/* Event sinks — safe to call from any task (they only queue). */
void ui_post_link_event(link_event_t evt, uint8_t slot);
void ui_post_input_event(input_event_t evt, uint8_t arg);

/* Dongle-provisioning progress (from flasher.c). */
typedef enum {
    UI_FLASH_IDLE = 0,
    UI_FLASH_BUSY,
    UI_FLASH_DONE,
    UI_FLASH_FAIL,
} ui_flash_state_t;

void ui_flasher_state(ui_flash_state_t state);
