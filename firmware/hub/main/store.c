/* espkvm hub — NVS persistence. */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include "kvm_crypto.h"
#include "store.h"

static const char *TAG = "store";
static nvs_handle_t s_nvs;

static uint8_t s_pmk[KVM_KEY_LEN];
static uint32_t s_epoch;
static hub_pairing_t s_pairs[KVM_MAX_SLOTS];
static uint8_t s_last_slot = 0;

#define NS "espkvm"

/* Blob stored per slot: mac[6] + lmk[16] + name[13] */
typedef struct __attribute__((packed)) {
    uint8_t mac[6];
    uint8_t lmk[KVM_KEY_LEN];
    char    name[KVM_NAME_LEN + 1];
} slot_blob_t;

static void slot_key(char *buf, size_t n, uint8_t slot)
{
    snprintf(buf, n, "s%u", slot);
}

esp_err_t store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &s_nvs));

    /* PMK: generate once, keep forever (dongles receive it at pairing). */
    size_t len = sizeof(s_pmk);
    if (nvs_get_blob(s_nvs, "pmk", s_pmk, &len) != ESP_OK ||
        len != sizeof(s_pmk)) {
        kvm_rand(s_pmk, sizeof(s_pmk));
        ESP_ERROR_CHECK(nvs_set_blob(s_nvs, "pmk", s_pmk, sizeof(s_pmk)));
        ESP_LOGI(TAG, "generated new PMK");
    }

    /* Anti-replay epoch: increment on every boot, persist immediately so a
     * crash can never reuse an epoch. */
    nvs_get_u32(s_nvs, "epoch", &s_epoch);
    s_epoch++;
    ESP_ERROR_CHECK(nvs_set_u32(s_nvs, "epoch", s_epoch));

    uint8_t last = 0;
    if (nvs_get_u8(s_nvs, "last_slot", &last) == ESP_OK &&
        last < KVM_MAX_SLOTS) {
        s_last_slot = last;
    }

    for (uint8_t i = 0; i < KVM_MAX_SLOTS; i++) {
        char key[8];
        slot_key(key, sizeof(key), i);
        slot_blob_t b;
        len = sizeof(b);
        if (nvs_get_blob(s_nvs, key, &b, &len) == ESP_OK && len == sizeof(b)) {
            s_pairs[i].used = true;
            memcpy(s_pairs[i].mac, b.mac, 6);
            memcpy(s_pairs[i].lmk, b.lmk, KVM_KEY_LEN);
            memcpy(s_pairs[i].name, b.name, sizeof(b.name));
            s_pairs[i].name[KVM_NAME_LEN] = '\0';
        }
    }

    ESP_ERROR_CHECK(nvs_commit(s_nvs));
    ESP_LOGI(TAG, "epoch %lu, last slot %u", (unsigned long)s_epoch, s_last_slot);
    return ESP_OK;
}

const uint8_t *store_pmk(void)
{
    return s_pmk;
}

uint32_t store_epoch(void)
{
    return s_epoch;
}

const hub_pairing_t *store_pairing(uint8_t slot)
{
    if (slot >= KVM_MAX_SLOTS || !s_pairs[slot].used) {
        return NULL;
    }
    return &s_pairs[slot];
}

esp_err_t store_save_pairing(uint8_t slot, const uint8_t mac[6],
                             const uint8_t lmk[KVM_KEY_LEN],
                             const char *name)
{
    if (slot >= KVM_MAX_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    slot_blob_t b = {0};
    memcpy(b.mac, mac, 6);
    memcpy(b.lmk, lmk, KVM_KEY_LEN);
    strncpy(b.name, name ? name : "", KVM_NAME_LEN);

    char key[8];
    slot_key(key, sizeof(key), slot);
    esp_err_t err = nvs_set_blob(s_nvs, key, &b, sizeof(b));
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err == ESP_OK) {
        s_pairs[slot].used = true;
        memcpy(s_pairs[slot].mac, mac, 6);
        memcpy(s_pairs[slot].lmk, lmk, KVM_KEY_LEN);
        memcpy(s_pairs[slot].name, b.name, sizeof(b.name));
        s_pairs[slot].name[KVM_NAME_LEN] = '\0';
    }
    return err;
}

esp_err_t store_erase_pairing(uint8_t slot)
{
    if (slot >= KVM_MAX_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[8];
    slot_key(key, sizeof(key), slot);
    nvs_erase_key(s_nvs, key);
    esp_err_t err = nvs_commit(s_nvs);
    memset(&s_pairs[slot], 0, sizeof(s_pairs[slot]));
    return err;
}

uint8_t store_slot_for_mac(const uint8_t mac[6])
{
    for (uint8_t i = 0; i < KVM_MAX_SLOTS; i++) {
        if (s_pairs[i].used && memcmp(s_pairs[i].mac, mac, 6) == 0) {
            return i;
        }
    }
    return KVM_SLOT_NONE;
}

uint8_t store_free_slot(void)
{
    for (uint8_t i = 0; i < KVM_MAX_SLOTS; i++) {
        if (!s_pairs[i].used) {
            return i;
        }
    }
    return KVM_SLOT_NONE;
}

uint8_t store_last_slot(void)
{
    return s_last_slot;
}

void store_set_last_slot(uint8_t slot)
{
    if (slot >= KVM_MAX_SLOTS || slot == s_last_slot) {
        return;
    }
    s_last_slot = slot;
    /* Written only on slot switch — negligible flash wear, and it is what
     * lets a rebooted hub come back on the machine you were using. */
    if (nvs_set_u8(s_nvs, "last_slot", slot) == ESP_OK) {
        nvs_commit(s_nvs);
    }
}
