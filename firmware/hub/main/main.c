/*
 * espkvm hub — entry point (ESP32-S3 DevKitC-1).
 *
 * Wiring (defaults, change in menuconfig → espkvm hub):
 *   SSD1306 OLED : SDA=GPIO8, SCL=GPIO9, addr 0x3C
 *   EC11 encoder : A=GPIO4, B=GPIO5, switch=GPIO6 (all to GND, pull-ups on)
 *   Keyboard     : USB-OTG port (see docs/BUILD.md for the 5 V note)
 *
 * SPDX-License-Identifier: MIT
 */

#include "esp_log.h"
#include "driver/gpio.h"

#include "store.h"
#include "oled.h"
#include "link.h"
#include "input.h"
#include "usb_kbd.h"
#include "ui.h"

static const char *TAG = "main";

/* Glue: decoded USB reports go to the input pipeline. */
static void on_usb_input(const kvm_hidp_out_t *ev)
{
    input_submit(ev);
}

void app_main(void)
{
#if CONFIG_ESPKVM_PWR_EN_GPIO >= 0
    /* Boards like the T-Embed gate display/peripheral power behind a
     * GPIO; enable it before anything tries to talk to the panel. */
    gpio_config_t pwr = {
        .pin_bit_mask = 1ULL << CONFIG_ESPKVM_PWR_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwr);
    gpio_set_level(CONFIG_ESPKVM_PWR_EN_GPIO, 1);
#endif

    ESP_ERROR_CHECK(store_init());

#if !CONFIG_ESPKVM_HUB_BOARD_HEADLESS
    /* Display is non-fatal: a hub without one still switches via
     * hotkeys and the button. */
    if (oled_init() != ESP_OK) {
        ESP_LOGW(TAG, "display not found — running blind");
    }
#endif

    ESP_ERROR_CHECK(link_init(ui_post_link_event));
    ESP_ERROR_CHECK(ui_init());
    ESP_ERROR_CHECK(input_init(ui_post_input_event));
    ESP_ERROR_CHECK(usb_kbd_init(on_usb_input));

    ESP_LOGI(TAG, "espkvm hub ready (active slot %u)", link_active_slot());
}
