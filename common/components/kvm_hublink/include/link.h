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

/* STA-mode init (default): the hub joins no network, it just needs a
 * Wi-Fi interface up to run ESP-NOW on CONFIG_ESPKVM_CHANNEL. Used by
 * the USB-host hub variants (DevKit/T-Embed/headless). */
esp_err_t link_init(link_event_cb_t cb);

/* AP-mode init: the hub also hosts its own Wi-Fi network on `channel`
 * (open if ap_pass is NULL/empty, else WPA2-PSK — 8-63 chars) so a
 * phone can join and drive it directly (see firmware/hub-air). ESP-NOW
 * rides the same radio/channel as the AP; both share one interface. */
esp_err_t link_init_ap(const char *ap_ssid, const char *ap_pass,
                       uint8_t channel, uint8_t max_clients,
                       link_event_cb_t cb);

/* Local slot: this hub board IS one of the computers' HID devices (see
 * firmware/hub-dongle — a dongle that doubles as the hub). Input for the
 * local slot is handed to these callbacks instead of ESP-NOW; `online`
 * feeds link_slot_online (e.g. usb_dev_mounted). Call after store_init()
 * and BEFORE link_init*() so the pairing-table entry and peer setup are
 * consistent. The slot is self-assigned: an existing entry carrying this
 * board's MAC is reused, otherwise the lowest free slot is claimed and
 * persisted under `name`. */
typedef struct {
    void (*kbd)(uint8_t mods, const uint8_t keys[6]);
    void (*mouse)(uint8_t buttons, int16_t dx, int16_t dy,
                  int8_t wheel, int8_t pan);
    void (*consumer)(uint16_t usage);
    void (*release_all)(void);
    bool (*online)(void);
} link_local_ops_t;

esp_err_t link_set_local(const char *name, const link_local_ops_t *ops);
uint8_t link_local_slot(void);           /* KVM_SLOT_NONE if not set     */

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
