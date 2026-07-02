/*
 * espkvm dongle — entry point.
 *
 * Boot order matters: USB first so the host sees a keyboard within
 * milliseconds of plug-in (BIOS menus!), then radio, then the tiny bit of
 * GPIO glue for the BOOT button and status LED.
 *
 * LED patterns (onboard LED, GPIO15 on the S2 Mini):
 *   fast blink (8 Hz)  — pairing mode, waiting for the hub
 *   slow blink (2 Hz)  — unpaired, press BOOT while hub is in pairing mode
 *   solid on           — paired + USB mounted (normal operation)
 *   long blink (0.5 Hz)— paired but USB not mounted / host suspended
 *
 * BOOT button: short press = enter pairing mode, hold 5 s = factory reset.
 *
 * SPDX-License-Identifier: MIT
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "store.h"
#include "usb_dev.h"
#include "link.h"

static const char *TAG = "main";

#define BTN_GPIO  CONFIG_ESPKVM_BTN_GPIO
#define LED_GPIO  CONFIG_ESPKVM_LED_GPIO

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
                link_factory_reset();   /* wipes NVS + reboots */
            }
        } else {
            if (was_down && held_ms >= BTN_DEBOUNCE_MS &&
                held_ms < BTN_FACTORY_MS) {
                ESP_LOGI(TAG, "BOOT pressed — entering pairing mode");
                link_start_pairing();
            }
            held_ms = 0;
            reset_fired = false;
        }
        was_down = down;
    }
}

#if CONFIG_ESPKVM_LED_GPIO >= 0
static void led_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);

    uint32_t t = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(25));
        t += 25;

        bool on;
        switch (link_state()) {
        case DLINK_PAIRING:
            on = (t / 60) % 2;              /* ~8 Hz */
            break;
        case DLINK_UNPAIRED:
            on = (t / 250) % 2;             /* 2 Hz */
            break;
        case DLINK_RUN:
        default:
            on = usb_dev_mounted() ? true   /* solid = all good */
                                   : ((t / 1000) % 2);
            break;
        }
        gpio_set_level(LED_GPIO, on);
    }
}
#endif /* CONFIG_ESPKVM_LED_GPIO >= 0 */

void app_main(void)
{
    ESP_ERROR_CHECK(store_init());

    /* USB before Wi-Fi: enumerate as a keyboard ASAP. */
    ESP_ERROR_CHECK(usb_dev_init());
    ESP_ERROR_CHECK(link_init());

    xTaskCreate(button_task, "btn", 3072, NULL, 5, NULL);
#if CONFIG_ESPKVM_LED_GPIO >= 0
    xTaskCreate(led_task, "led", 2048, NULL, 3, NULL);
#endif

    ESP_LOGI(TAG, "espkvm dongle ready");
}
