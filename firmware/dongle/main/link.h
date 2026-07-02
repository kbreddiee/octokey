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

/* Milliseconds since the last packet accepted from the hub (for LED/LCD). */
uint32_t link_ms_since_rx(void);

/* Our slot number (0-9), or KVM_SLOT_NONE when unpaired. */
uint8_t link_slot(void);

/* True while the hub says we are the selected target (KVM_FLAG_ACTIVE on
 * the last accepted packet). Combine with link_ms_since_rx() for staleness. */
bool link_is_active(void);
