/* espkvm dongle — NVS persistence. */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include "store.h"

static const char *TAG = "store";
static nvs_handle_t s_nvs;

#define NS        "espkvm"
#define KEY_PAIR  "pair"
#define KEY_EPOCH "hepoch"

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
    return nvs_open(NS, NVS_READWRITE, &s_nvs);
}

bool store_get_pairing(dongle_pairing_t *p)
{
    size_t len = sizeof(*p);
    esp_err_t err = nvs_get_blob(s_nvs, KEY_PAIR, p, &len);
    return err == ESP_OK && len == sizeof(*p);
}

esp_err_t store_save_pairing(const dongle_pairing_t *p)
{
    esp_err_t err = nvs_set_blob(s_nvs, KEY_PAIR, p, sizeof(*p));
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    return err;
}

uint32_t store_get_epoch(void)
{
    uint32_t e = 0;
    nvs_get_u32(s_nvs, KEY_EPOCH, &e);
    return e;
}

void store_set_epoch(uint32_t epoch)
{
    /* Written at most once per hub boot (when a fresh epoch is first
     * accepted), so flash wear is a non-issue. */
    if (nvs_set_u32(s_nvs, KEY_EPOCH, epoch) == ESP_OK) {
        nvs_commit(s_nvs);
    } else {
        ESP_LOGW(TAG, "failed to persist epoch");
    }
}

esp_err_t store_clear(void)
{
    nvs_erase_key(s_nvs, KEY_PAIR);
    nvs_erase_key(s_nvs, KEY_EPOCH);
    return nvs_commit(s_nvs);
}
