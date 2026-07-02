/* espkvm hub — USB dongle provisioning (flash a T-Dongle-S3 in-place). */
/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Header prefixed to the dongle firmware image stored in the `dimage`
 * partition (written by tools/gen_dongle_pack.py). */
#define KVM_DIMAGE_MAGIC 0x444D564Bu   /* "KVMD" little-endian */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t len;          /* payload bytes following this header        */
    uint32_t crc32;        /* CRC-32 of the payload                      */
    uint32_t flash_addr;   /* target flash offset (0 for merged images)  */
    uint16_t fw_ver;
    uint8_t  reserved[14]; /* header is 32 bytes total                   */
} kvm_dimage_hdr_t;

_Static_assert(sizeof(kvm_dimage_hdr_t) == 32, "dimage header size");

/* Starts the provisioning watcher: when the keyboard port has no HID
 * device and an Espressif ROM bootloader enumerates instead (a dongle
 * plugged in with BOOT held), the stored image is flashed into it. */
esp_err_t flasher_init(void);
