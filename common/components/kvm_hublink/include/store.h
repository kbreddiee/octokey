/* espkvm hub — NVS persistence: PMK, boot epoch, pairing table, last slot. */
/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "kvm_proto.h"

typedef struct {
    bool    used;
    uint8_t mac[6];
    uint8_t lmk[KVM_KEY_LEN];
    char    name[KVM_NAME_LEN + 1];   /* NUL-terminated display name */
} hub_pairing_t;

esp_err_t store_init(void);

/* Project-wide ESP-NOW PMK: random, generated once at first boot. */
const uint8_t *store_pmk(void);

/* Boot counter for anti-replay; already incremented for this boot. */
uint32_t store_epoch(void);

const hub_pairing_t *store_pairing(uint8_t slot);
esp_err_t store_save_pairing(uint8_t slot, const uint8_t mac[6],
                             const uint8_t lmk[KVM_KEY_LEN],
                             const char *name);
esp_err_t store_erase_pairing(uint8_t slot);

/* Slot of an already-paired MAC, or KVM_SLOT_NONE. */
uint8_t store_slot_for_mac(const uint8_t mac[6]);
/* Lowest unused slot, or KVM_SLOT_NONE if all 10 are taken. */
uint8_t store_free_slot(void);

uint8_t store_last_slot(void);
void store_set_last_slot(uint8_t slot);
