/* espkvm hub — ESP-NOW transmit path, pairing (hub side), link health. */
/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    LINK_EVT_STATUS,        /* a slot went online/offline                */
    LINK_EVT_SWITCHED,      /* active slot changed (slot = new)          */
    LINK_EVT_PAIRED,        /* pairing completed (slot = new dongle)     */
    LINK_EVT_PAIR_FAIL,     /* pairing failed (all slots full, ...)      */
    LINK_EVT_PAIR_TIMEOUT,  /* pairing window elapsed / cancelled        */
} link_event_t;

/* Called from the link task (never from ISR); implementations must only
 * queue, not block. */
typedef void (*link_event_cb_t)(link_event_t evt, uint8_t slot);

esp_err_t link_init(link_event_cb_t cb);

/* Input forwarding — silently dropped when no slot is active. */
void link_send_kbd(uint8_t mods, const uint8_t keys[6]);
void link_send_mouse(uint8_t buttons, int16_t dx, int16_t dy,
                     int8_t wheel, int8_t pan);
void link_send_consumer(uint16_t usage);

/* Switch the active target. Sends RELEASE_ALL to the previous slot first
 * so nothing stays held down on the machine you are leaving. */
void link_switch(uint8_t slot);

uint8_t link_active_slot(void);          /* KVM_SLOT_NONE if none        */
bool link_slot_online(uint8_t slot);

void link_start_pairing(void);           /* 30 s beacon window           */
void link_cancel_pairing(void);
bool link_pairing_active(void);

/* Best-effort UNPAIR to the dongle, then forget the slot locally. */
void link_unpair(uint8_t slot);
