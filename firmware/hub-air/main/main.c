/*
 * espkvm hub-air — entry point.
 *
 * A radio-only hub: no USB host, no display, no encoder. It hosts a
 * small Wi-Fi network (CONFIG_ESPKVM_AP_SSID) and a phone browser at
 * http://192.168.4.1/ becomes the keyboard/mouse/switcher — see
 * main/web/index.html for the client and webui.c for the bridge to
 * kvm_hublink (the same pairing/ESP-NOW code the USB-host hubs use).
 *
 * Runs on any ESP32/S2/S3/C3 board with just a power source; no wiring.
 *
 * SPDX-License-Identifier: MIT
 */

#include "esp_log.h"

#include "store.h"
#include "link.h"
#include "webui.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_ERROR_CHECK(store_init());
    ESP_ERROR_CHECK(link_init_ap(CONFIG_ESPKVM_AP_SSID,
                                 CONFIG_ESPKVM_AP_PASSWORD,
                                 CONFIG_ESPKVM_CHANNEL,
                                 CONFIG_ESPKVM_AP_MAX_CLIENTS,
                                 webui_link_event));
    ESP_ERROR_CHECK(webui_init());

    ESP_LOGI(TAG, "espkvm hub-air ready — Wi-Fi \"%s\", active slot %u",
             CONFIG_ESPKVM_AP_SSID, link_active_slot());
}
