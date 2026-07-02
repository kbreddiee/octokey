/* espkvm dongle — NVS persistence (pairing + anti-replay epoch). */
/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "kvm_proto.h"

typedef struct {
    uint8_t hub_mac[6];
    uint8_t pmk[KVM_KEY_LEN];
    uint8_t lmk[KVM_KEY_LEN];
    uint8_t slot;
} dongle_pairing_t;

esp_err_t store_init(void);

/* Returns true and fills `p` if a pairing is persisted. */
bool store_get_pairing(dongle_pairing_t *p);
esp_err_t store_save_pairing(const dongle_pairing_t *p);

/* Highest hub epoch ever accepted (replay floor across dongle reboots). */
uint32_t store_get_epoch(void);
void store_set_epoch(uint32_t epoch);

/* Factory reset: erase pairing + epoch. */
esp_err_t store_clear(void);
