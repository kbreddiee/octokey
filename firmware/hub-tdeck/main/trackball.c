/*
 * espkvm hub-tdeck — trackball driver.
 *
 * The trackball is 4 tactile switches to GND (one per direction, pulsed
 * by the ball's mechanical detents — not a smooth analog sensor) plus a
 * center push-button on GPIO0/BOOT (same "reused post-boot" pattern the
 * T-Embed hub already uses for its encoder button). Direction pulses are
 * accumulated in ISRs and flushed to relative mouse movement every 20 ms
 * — or, while the keyboard's Symbol key is held, converted to arrow-key
 * taps instead (a partial workaround for this keyboard having no arrow
 * keys of its own; see kbd_i2c.c).
 *
 * The center-button gesture state machine (short=click, hold 3s=pair,
 * triple-click+confirm=forget) is a direct port of ui_headless.c's
 * button_task from the custom-PCB hub — same proven logic, same GPIO
 * polling style, just retargeted from a dedicated MODE button to this
 * board's trackball click.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "kvm_proto.h"
#include "store.h"
#include "link.h"
#include "disp.h"
#include "kbd_i2c.h"
#include "trackball.h"

static const char *TAG = "trackball";

#define STEP           CONFIG_ESPKVM_TBALL_STEP
#define FLUSH_MS       20
#define CLICK_MAX_MS   400
#define MULTI_GAP_MS   350
#define HOLD_PAIR_MS   3000
#define PAIR_WINDOW_S  30
#define SYMBOL_POLL_MS 150

static volatile uint8_t s_up, s_down, s_left, s_right;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR isr_up(void *a)    { (void)a; portENTER_CRITICAL_ISR(&s_mux); if (s_up    < 250) s_up++;    portEXIT_CRITICAL_ISR(&s_mux); }
static void IRAM_ATTR isr_down(void *a)  { (void)a; portENTER_CRITICAL_ISR(&s_mux); if (s_down  < 250) s_down++;  portEXIT_CRITICAL_ISR(&s_mux); }
static void IRAM_ATTR isr_left(void *a)  { (void)a; portENTER_CRITICAL_ISR(&s_mux); if (s_left  < 250) s_left++;  portEXIT_CRITICAL_ISR(&s_mux); }
static void IRAM_ATTR isr_right(void *a) { (void)a; portENTER_CRITICAL_ISR(&s_mux); if (s_right < 250) s_right++; portEXIT_CRITICAL_ISR(&s_mux); }

static void tap_arrow(uint8_t usage)
{
    uint8_t keys[6] = {usage, 0, 0, 0, 0, 0};
    static const uint8_t none[6] = {0};
    link_send_kbd(0, keys);
    vTaskDelay(pdMS_TO_TICKS(30));
    link_send_kbd(0, none);
}

static void tap_click(uint8_t bit)
{
    link_send_mouse(bit, 0, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(40));
    link_send_mouse(0, 0, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Center-button gestures (ported from ui_headless.c's button_task)    */
/* ------------------------------------------------------------------ */

static void gesture_task(void *arg)
{
    (void)arg;
    int held_ms = 0, gap_ms = 0, clicks = 0;
    bool was_down = false, hold_fired = false, forget_armed = false;
    int64_t forget_deadline_us = 0, pair_end_us = 0;
    bool pairing = false;
    int64_t next_symbol_poll_us = 0;
    bool symbol_held = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FLUSH_MS));
        int64_t now = esp_timer_get_time();

        /* --- flush accumulated trackball pulses --------------------- */
        uint8_t up, down, left, right;
        portENTER_CRITICAL(&s_mux);
        up = s_up; down = s_down; left = s_left; right = s_right;
        s_up = s_down = s_left = s_right = 0;
        portEXIT_CRITICAL(&s_mux);

        if (now >= next_symbol_poll_us) {
            symbol_held = kbd_i2c_symbol_held();
            next_symbol_poll_us = now + (int64_t)SYMBOL_POLL_MS * 1000;
        }

        if (up || down || left || right) {
            if (symbol_held) {
                for (uint8_t i = 0; i < up; i++)    tap_arrow(0x52);
                for (uint8_t i = 0; i < down; i++)  tap_arrow(0x51);
                for (uint8_t i = 0; i < left; i++)  tap_arrow(0x50);
                for (uint8_t i = 0; i < right; i++) tap_arrow(0x4F);
            } else {
                int16_t dx = (int16_t)(right - left) * STEP;
                int16_t dy = (int16_t)(down - up) * STEP;
                link_send_mouse(0, dx, dy, 0, 0);
            }
        }

        /* --- center button: short=click, hold 3s=pair, 3x+confirm=forget */
        bool down_now = gpio_get_level(CONFIG_ESPKVM_TBALL_CLICK) == 0;
        if (down_now) {
            held_ms += FLUSH_MS;
            if (held_ms >= HOLD_PAIR_MS && !hold_fired) {
                hold_fired = true;
                clicks = 0;
                link_start_pairing();
                pairing = true;
                pair_end_us = now + (int64_t)PAIR_WINDOW_S * 1000000;
                disp_set_pairing(true, PAIR_WINDOW_S);
            }
        } else {
            if (was_down && !hold_fired && held_ms >= 20 && held_ms < CLICK_MAX_MS) {
                clicks++;
                gap_ms = 0;
            }
            held_ms = 0;
            hold_fired = false;
        }
        was_down = down_now;

        if (clicks > 0 && !down_now) {
            gap_ms += FLUSH_MS;
            if (gap_ms > MULTI_GAP_MS) {
                if (forget_armed && clicks == 1) {
                    uint8_t slot = link_active_slot();
                    if (slot != KVM_SLOT_NONE) {
                        link_unpair(slot);
                        disp_toast("slot forgotten");
                    }
                    forget_armed = false;
                } else if (clicks == 1) {
                    tap_click(1);   /* left click */
                } else if (clicks == 3) {
                    if (link_active_slot() != KVM_SLOT_NONE) {
                        forget_armed = true;
                        forget_deadline_us = now + 3000000;
                        disp_toast("click once more to forget");
                    }
                }
                clicks = 0;
            }
        }
        if (forget_armed && now > forget_deadline_us) {
            forget_armed = false;
        }

        if (pairing && now > pair_end_us) {
            pairing = false;   /* link.c's own 30s window has also
                                 * elapsed; LINK_EVT_PAIR_TIMEOUT (handled
                                 * in main.c) clears the display. */
        } else if (pairing) {
            disp_set_pairing(true, (int)((pair_end_us - now) / 1000000));
        }
    }
}

esp_err_t trackball_init(void)
{
    gpio_config_t dirs = {
        .pin_bit_mask = (1ULL << CONFIG_ESPKVM_TBALL_UP) |
                        (1ULL << CONFIG_ESPKVM_TBALL_DOWN) |
                        (1ULL << CONFIG_ESPKVM_TBALL_LEFT) |
                        (1ULL << CONFIG_ESPKVM_TBALL_RIGHT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&dirs));

    gpio_config_t click = {
        .pin_bit_mask = 1ULL << CONFIG_ESPKVM_TBALL_CLICK,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&click));

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(CONFIG_ESPKVM_TBALL_UP, isr_up, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(CONFIG_ESPKVM_TBALL_DOWN, isr_down, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(CONFIG_ESPKVM_TBALL_LEFT, isr_left, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(CONFIG_ESPKVM_TBALL_RIGHT, isr_right, NULL));

    xTaskCreate(gesture_task, "tball", 4096, NULL, 9, NULL);
    ESP_LOGI(TAG, "trackball up (U=%d D=%d L=%d R=%d click=%d)",
             CONFIG_ESPKVM_TBALL_UP, CONFIG_ESPKVM_TBALL_DOWN,
             CONFIG_ESPKVM_TBALL_LEFT, CONFIG_ESPKVM_TBALL_RIGHT,
             CONFIG_ESPKVM_TBALL_CLICK);
    return ESP_OK;
}
