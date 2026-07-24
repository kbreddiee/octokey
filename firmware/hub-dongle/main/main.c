/*
 * espkvm hub-dongle — entry point (LILYGO T-Dongle-S3 or any ESP32-S2/S3
 * stick with native USB).
 *
 * The dongle that doubles as the hub, so a 3-computer setup needs exactly
 * 3 sticks and no extra board:
 *   - USB HID device to the computer it's plugged into (the "local slot",
 *    served straight into TinyUSB — no radio hop, see link_set_local),
 *   - Wi-Fi AP + phone web UI (shared kvm_webui: join CONFIG_ESPKVM_AP_SSID,
 *     browse http://192.168.4.1/),
 *   - ESP-NOW hub to the other computers' plain dongles.
 *
 * Trade-off vs a dedicated hub-air stick: the whole system lives on this
 * stick's USB power — plug it into the machine that's always on.
 *
 * BOOT button: short press = open the pairing window, hold 5 s = factory
 * reset (wipe pairings + keys, reboot).
 *
 * SPDX-License-Identifier: MIT
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"

#include "store.h"
#include "link.h"
#include "webui.h"
#include "usb_dev.h"
#if CONFIG_ESPKVM_LCD
#include "lcd.h"
#endif

static const char *TAG = "main";

#define BTN_GPIO           CONFIG_ESPKVM_BTN_GPIO
#define BTN_POLL_MS        10
#define BTN_DEBOUNCE_MS    30
#define BTN_FACTORY_MS     5000

static void button_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   /* BOOT is active-low */
    };
    gpio_config(&io);

    int held_ms = 0;
    bool was_down = false;
    bool reset_fired = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
        bool down = gpio_get_level(BTN_GPIO) == 0;

        if (down) {
            held_ms += BTN_POLL_MS;
            if (held_ms >= BTN_FACTORY_MS && !reset_fired) {
                reset_fired = true;
                ESP_LOGW(TAG, "factory reset — wiping NVS");
                nvs_flash_erase();
                esp_restart();
            }
        } else {
            if (was_down && held_ms >= BTN_DEBOUNCE_MS &&
                held_ms < BTN_FACTORY_MS) {
                ESP_LOGI(TAG, "BOOT pressed — opening pairing window");
                link_start_pairing();
            }
            held_ms = 0;
            reset_fired = false;
        }
        was_down = down;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(store_init());

    /* USB before Wi-Fi: enumerate as a keyboard ASAP (BIOS menus!). */
    ESP_ERROR_CHECK(usb_dev_init());

    static const link_local_ops_t local_ops = {
        .kbd         = usb_dev_kbd,
        .mouse       = usb_dev_mouse,
        .consumer    = usb_dev_consumer,
        .release_all = usb_dev_release_all,
        .online      = usb_dev_mounted,
    };
    ESP_ERROR_CHECK(link_set_local(CONFIG_ESPKVM_LOCAL_NAME, &local_ops));

    ESP_ERROR_CHECK(link_init_ap(CONFIG_ESPKVM_AP_SSID,
                                 CONFIG_ESPKVM_AP_PASSWORD,
                                 CONFIG_ESPKVM_CHANNEL,
                                 CONFIG_ESPKVM_AP_MAX_CLIENTS,
                                 webui_link_event));
    ESP_ERROR_CHECK(webui_init());

    xTaskCreate(button_task, "btn", 3072, NULL, 5, NULL);
#if CONFIG_ESPKVM_LCD
    if (lcd_init() != ESP_OK) {
        ESP_LOGW(TAG, "LCD init failed — continuing without display");
    }
#endif

    ESP_LOGI(TAG, "espkvm hub-dongle ready — Wi-Fi \"%s\", local slot %u, "
             "active slot %u", CONFIG_ESPKVM_AP_SSID, link_local_slot(),
             link_active_slot());
}
