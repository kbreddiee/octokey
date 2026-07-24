/*
 * espkvm hub-tdeck — entry point (LILYGO T-Deck / T-Deck Plus).
 *
 * A fully self-contained hub: no phone, no PC, no external keyboard —
 * the board's own I2C keyboard and trackball are the input devices (see
 * kbd_i2c.c / trackball.c for exactly what they can and can't do; the
 * physical keyboard has no Ctrl/Alt/Win/arrows/Esc/Tab/F-keys, worked
 * around with remaps documented there and in docs/BUILD.md).
 *
 * It also hosts the same Wi-Fi AP + phone web UI as hub-air (shared
 * kvm_webui component): join CONFIG_ESPKVM_AP_SSID and browse to
 * http://192.168.4.1/ to type/mouse/switch from a phone as well.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "kvm_proto.h"
#include "store.h"
#include "link.h"
#include "webui.h"
#include "disp.h"
#include "kbd_i2c.h"
#include "touch.h"
#include "trackball.h"

static const char *TAG = "main";

static void on_link_event(link_event_t evt, uint8_t slot)
{
    webui_link_event(evt, slot);   /* keep connected phones in sync */

    switch (evt) {
    case LINK_EVT_PAIRED: {
        const hub_pairing_t *p = store_pairing(slot);
        char msg[32];
        snprintf(msg, sizeof(msg), "paired %s", p ? p->name : "dongle");
        disp_set_pairing(false, 0);
        disp_toast(msg);
        break;
    }
    case LINK_EVT_PAIR_FAIL:
        disp_set_pairing(false, 0);
        disp_toast("all 10 slots used");
        break;
    case LINK_EVT_PAIR_TIMEOUT:
        disp_set_pairing(false, 0);
        disp_toast("pairing timed out");
        break;
    case LINK_EVT_STATUS:
    case LINK_EVT_SWITCHED:
    default:
        break;   /* disp_task redraws from live link/store state anyway */
    }
}

void app_main(void)
{
    /* Board peripheral power rail (keyboard co-processor, trackball,
     * LCD) — must be driven high before touching I2C/SPI, then given
     * time to boot the keyboard's ESP32-C3 (LilyGO's own example waits
     * 500 ms here). */
    gpio_config_t pwr = {
        .pin_bit_mask = 1ULL << CONFIG_ESPKVM_POWERON_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwr);
    gpio_set_level(CONFIG_ESPKVM_POWERON_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_ERROR_CHECK(store_init());

    if (disp_init() != ESP_OK) {
        ESP_LOGW(TAG, "display init failed — continuing without it");
    }
    if (kbd_i2c_init() != ESP_OK) {
        ESP_LOGE(TAG, "keyboard co-processor not found — check wiring/power");
    }
    if (touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "touch panel not found — trackball is the only pointer");
    }
    ESP_ERROR_CHECK(trackball_init());
    ESP_ERROR_CHECK(link_init_ap(CONFIG_ESPKVM_AP_SSID,
                                 CONFIG_ESPKVM_AP_PASSWORD,
                                 CONFIG_ESPKVM_CHANNEL,
                                 CONFIG_ESPKVM_AP_MAX_CLIENTS,
                                 on_link_event));
    ESP_ERROR_CHECK(webui_init());

    ESP_LOGI(TAG, "espkvm hub-tdeck ready — Wi-Fi \"%s\", active slot %u",
             CONFIG_ESPKVM_AP_SSID, link_active_slot());
}
