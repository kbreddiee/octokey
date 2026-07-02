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
