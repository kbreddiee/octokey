/*
 * espkvm hub — headless UI (custom PCB: one button + one LED per slot).
 *
 * Implements the same interface as ui.c (ui.h) with no display at all:
 *
 *   button  short press          switch to the next paired slot
 *           hold 3 s             enter pairing mode (LED chase)
 *           triple-click         forget the active slot (after a warning
 *                                flash — release nothing to confirm)
 *
 *   LEDs    solid                the active slot
 *           2 Hz blink           active slot but its dongle is offline
 *           8 Hz blink           hotkey command mode (waiting for digit)
 *           chase animation      pairing mode
 *           5 quick flashes      pairing completed on that slot
 *           all-LED triple flash error/warning (slots full, forget warn)
 *
 * Per-slot detail lives on the dongles' own LCDs; the hub only needs to
 * answer "which machine am I typing into" — one lit LED does that.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "kvm_proto.h"
#include "store.h"
#include "link.h"
#include "input.h"
#include "ui.h"

static const char *TAG = "ui";

/* Slot LEDs 0..9. Matches hardware/hub-refdesign; edit to suit your PCB.
 * All are free, non-strapping pins on the ESP32-S3-WROOM-1 (incl. N16R8
 * octal-PSRAM modules). */
static const gpio_num_t LED_PINS[KVM_MAX_SLOTS] = {
    GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_8,
    GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13,
};

#define BTN_GPIO        CONFIG_ESPKVM_HUB_BTN_GPIO
#define TICK_MS         20
#define CLICK_MAX_MS    400
#define MULTI_GAP_MS    350
#define HOLD_PAIR_MS    3000
#define PAIR_WINDOW_S   30

typedef struct {
    uint8_t type;   /* 0 = link event, 1 = input event */
    uint8_t evt;
    uint8_t arg;
} ui_msg_t;

static QueueHandle_t s_q;

static struct {
    bool     pairing;
    int64_t  pair_end_us;
    bool     cmd_mode;
    uint8_t  flash_slot;       /* KVM_SLOT_NONE = no flash running       */
    int      flash_ticks;
    int      warn_ticks;       /* all-LED warning flash countdown        */
    bool     forget_armed;     /* triple-click detected, warning shown   */
} s_ui;

/* ------------------------------------------------------------------ */
/* LED rendering (called every TICK_MS)                                */
/* ------------------------------------------------------------------ */

static void leds_render(uint32_t tick)
{
    bool on[KVM_MAX_SLOTS] = {0};
    uint8_t active = link_active_slot();

    if (s_ui.warn_ticks > 0) {
        /* all-LED triple flash: 100 ms on / 100 ms off */
        bool lit = ((s_ui.warn_ticks / 5) % 2) == 1;
        for (int i = 0; i < KVM_MAX_SLOTS; i++) {
            on[i] = lit;
        }
        s_ui.warn_ticks--;
    } else if (s_ui.pairing) {
        on[(tick / 5) % KVM_MAX_SLOTS] = true;   /* chase */
    } else if (s_ui.flash_slot != KVM_SLOT_NONE) {
        on[s_ui.flash_slot] = ((s_ui.flash_ticks / 5) % 2) == 1;
        if (--s_ui.flash_ticks <= 0) {
            s_ui.flash_slot = KVM_SLOT_NONE;
        }
    } else if (active != KVM_SLOT_NONE) {
        if (s_ui.cmd_mode) {
            on[active] = (tick / 3) % 2;                  /* ~8 Hz */
        } else if (!link_slot_online(active)) {
            on[active] = (tick / 12) % 2;                 /* ~2 Hz */
        } else {
            on[active] = true;                            /* solid */
        }
    }
    /* No slots paired: heartbeat on LED 0 so the board looks alive. */
    if (active == KVM_SLOT_NONE && !s_ui.pairing && s_ui.warn_ticks == 0) {
        on[0] = (tick % 50) < 2;
    }

    for (int i = 0; i < KVM_MAX_SLOTS; i++) {
        gpio_set_level(LED_PINS[i], on[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Button gestures                                                    */
/* ------------------------------------------------------------------ */

static void next_slot(void)
{
    uint8_t cur = link_active_slot();
    uint8_t s = (cur == KVM_SLOT_NONE) ? 0 : cur;
    for (int i = 1; i <= KVM_MAX_SLOTS; i++) {
        uint8_t cand = (uint8_t)((s + i) % KVM_MAX_SLOTS);
        if (store_pairing(cand)) {
            link_switch(cand);
            return;
        }
    }
    s_ui.warn_ticks = 30;   /* nothing paired */
}

static void on_clicks(int count)
{
    if (s_ui.pairing) {
        /* any click cancels pairing mode */
        link_cancel_pairing();
        s_ui.pairing = false;
        return;
    }
    if (count == 1) {
        next_slot();
    } else if (count == 3) {
        /* Forget the active slot: warn first, execute on the next single
         * click within 3 s (so a stray triple-click alone is harmless). */
        if (!s_ui.forget_armed && link_active_slot() != KVM_SLOT_NONE) {
            s_ui.forget_armed = true;
            s_ui.warn_ticks = 30;
            ESP_LOGW(TAG, "triple-click: click once more to forget slot %u",
                     link_active_slot());
        }
    }
}

static void on_hold(void)
{
    link_start_pairing();
    s_ui.pairing = true;
    s_ui.pair_end_us = esp_timer_get_time() + (int64_t)PAIR_WINDOW_S * 1000000;
}

/* ------------------------------------------------------------------ */
/* Main task                                                          */
/* ------------------------------------------------------------------ */

static void ui_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    int held_ms = 0, gap_ms = 0, clicks = 0;
    bool was_down = false, hold_fired = false;
    int64_t forget_deadline_us = 0;

    for (;;) {
        ui_msg_t m;
        while (xQueueReceive(s_q, &m, 0) == pdTRUE) {
            if (m.type == 0) {
                switch ((link_event_t)m.evt) {
                case LINK_EVT_PAIRED:
                    s_ui.pairing = false;
                    s_ui.flash_slot = m.arg;
                    s_ui.flash_ticks = 50;
                    break;
                case LINK_EVT_PAIR_TIMEOUT:
                    s_ui.pairing = false;
                    break;
                case LINK_EVT_PAIR_FAIL:
                    s_ui.warn_ticks = 30;
                    break;
                default:
                    break;
                }
            } else {
                switch ((input_event_t)m.evt) {
                case INPUT_EVT_CMD_ON:  s_ui.cmd_mode = true;  break;
                case INPUT_EVT_CMD_OFF: s_ui.cmd_mode = false; break;
                case INPUT_EVT_CMD_INVALID: s_ui.warn_ticks = 30; break;
                default: break;
                }
            }
        }

        /* --- button FSM ------------------------------------------- */
        bool down = gpio_get_level(BTN_GPIO) == 0;
        if (down) {
            held_ms += TICK_MS;
            if (held_ms >= HOLD_PAIR_MS && !hold_fired) {
                hold_fired = true;
                clicks = 0;
                on_hold();
            }
        } else {
            if (was_down && !hold_fired && held_ms >= 40 &&
                held_ms < CLICK_MAX_MS) {
                clicks++;
                gap_ms = 0;
            }
            held_ms = 0;
            hold_fired = false;
        }
        was_down = down;

        if (clicks > 0 && !down) {
            gap_ms += TICK_MS;
            if (gap_ms > MULTI_GAP_MS) {
                if (s_ui.forget_armed && clicks == 1) {
                    /* confirmation click for triple-click forget */
                    uint8_t slot = link_active_slot();
                    if (slot != KVM_SLOT_NONE) {
                        link_unpair(slot);
                        s_ui.warn_ticks = 30;
                        ESP_LOGW(TAG, "slot %u forgotten", slot);
                    }
                    s_ui.forget_armed = false;
                } else {
                    on_clicks(clicks);
                }
                clicks = 0;
            }
        }
        /* Arm the 3 s confirmation window when triple-click fires, and
         * disarm when it lapses (order matters: set before checking). */
        if (s_ui.forget_armed && forget_deadline_us == 0) {
            forget_deadline_us = esp_timer_get_time() + 3000000;
        }
        if (s_ui.forget_armed && forget_deadline_us != 0 &&
            esp_timer_get_time() > forget_deadline_us) {
            s_ui.forget_armed = false;
        }
        if (!s_ui.forget_armed) {
            forget_deadline_us = 0;
        }

        /* --- pairing window countdown ------------------------------ */
        if (s_ui.pairing && esp_timer_get_time() > s_ui.pair_end_us) {
            s_ui.pairing = false;
        }

        leds_render(tick++);
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

/* ------------------------------------------------------------------ */
/* API (same as ui.c)                                                 */
/* ------------------------------------------------------------------ */

void ui_post_link_event(link_event_t evt, uint8_t slot)
{
    ui_msg_t m = { .type = 0, .evt = (uint8_t)evt, .arg = slot };
    xQueueSend(s_q, &m, 0);
}

void ui_post_input_event(input_event_t evt, uint8_t arg)
{
    ui_msg_t m = { .type = 1, .evt = (uint8_t)evt, .arg = arg };
    xQueueSend(s_q, &m, 0);
}

esp_err_t ui_init(void)
{
    s_q = xQueueCreate(16, sizeof(ui_msg_t));
    if (!s_q) {
        return ESP_ERR_NO_MEM;
    }

    uint64_t mask = 0;
    for (int i = 0; i < KVM_MAX_SLOTS; i++) {
        mask |= 1ULL << LED_PINS[i];
    }
    gpio_config_t leds = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&leds));

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

    xTaskCreate(ui_task, "ui", 4096, NULL, 6, NULL);
    ESP_LOGI(TAG, "headless UI up (button GPIO%d, 10 slot LEDs)", BTN_GPIO);
    return ESP_OK;
}
