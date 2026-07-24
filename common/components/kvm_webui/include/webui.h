/* espkvm — phone web UI: HTTP + WebSocket bridge to kvm_hublink. */
/* SPDX-License-Identifier: MIT */
#pragma once

#include "esp_err.h"
#include "link.h"

/* Starts the HTTP server (serves the embedded single-page app at "/" and
 * the WebSocket control channel at "/ws"). Call after link_init_ap(). */
esp_err_t webui_init(void);

/* link_event_cb_t — pass this straight to link_init_ap(). Runs on the
 * kvm_hublink task; pushes an updated slot snapshot (and, for pairing
 * outcomes, a short toast message) to every connected phone. */
void webui_link_event(link_event_t evt, uint8_t slot);
