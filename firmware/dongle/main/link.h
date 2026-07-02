/* espkvm dongle — ESP-NOW receive path + pairing state machine. */
/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    DLINK_UNPAIRED,   /* no pairing in NVS; waiting for BOOT press       */
    DLINK_PAIRING,    /* scanning for a hub beacon / mid-handshake       */
    DLINK_RUN,        /* paired; replaying hub input onto USB            */
} dlink_state_t;

esp_err_t link_init(void);

dlink_state_t link_state(void);

/* BOOT short-press: (re)enter pairing mode. */
void link_start_pairing(void);

/* BOOT held 5 s: wipe pairing and reboot. */
void link_factory_reset(void);

/* Milliseconds since the last packet accepted from the hub (for LED). */
uint32_t link_ms_since_rx(void);
