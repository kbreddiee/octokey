/*
 * espkvm hub — encoder + OLED user interface.
 *
 * Controls:
 *   rotate            move the selection cursor over paired slots
 *   click             switch to the selected slot
 *   double-click      menu (pair new dongle / forget slot / cancel)
 *   hold 3 s          enter pairing mode directly
 *
 * The encoder is decoded with a quadrature state table on both edges of
 * A and B — immune to contact bounce by construction (invalid transitions
 * contribute 0) — accumulating quarter-steps into one event per detent.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "kvm_proto.h"
#include "store.h"
#include "usb_kbd.h"
#include "oled.h"
#include "ui.h"

static const char *TAG = "ui";

#define ENC_A   CONFIG_ESPKVM_ENC_A
#define ENC_B   CONFIG_ESPKVM_ENC_B
#define ENC_SW  CONFIG_ESPKVM_ENC_SW

#define LONG_PRESS_MS   3000
#define DBL_CLICK_MS    350
#define TOAST_MS        2500
#define PAIR_WINDOW_S   30

typedef enum {
    UMSG_ROT, UMSG_CLICK, UMSG_DBL, UMSG_LONG,
    UMSG_LINK, UMSG_INPUT, UMSG_TICK,
} umsg_type_t;

typedef struct {
    uint8_t type;
    int8_t  delta;
    uint8_t evt;
    uint8_t arg;
} ui_msg_t;

static QueueHandle_t s_q;

typedef enum { SCR_MAIN, SCR_MENU, SCR_PAIRING } screen_t;

static struct {
    screen_t screen;
    uint8_t  sel;            /* selection cursor (a slot number)         */
    uint8_t  menu_idx;
    bool     cmd_mode;       /* hotkey command mode indicator            */
    char     toast[22];
    int64_t  toast_until_us;
    int64_t  pair_end_us;
} s_ui;

/* ------------------------------------------------------------------ */
/* Encoder ISR — quadrature state table                                */
/* ------------------------------------------------------------------ */

/* Index: (prev_state << 2) | current_state. Valid Gray-code transitions
 * yield ±1 quarter-step; bounces and glitches yield 0. */
static const int8_t QTABLE[16] = {
    0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0
};

static volatile uint8_t s_enc_state;
static volatile int8_t s_enc_accum;

static void IRAM_ATTR enc_isr(void *arg)
{
    (void)arg;
    uint8_t ab = ((uint8_t)gpio_get_level(ENC_A) << 1) |
                 (uint8_t)gpio_get_level(ENC_B);
    s_enc_state = ((s_enc_state << 2) | ab) & 0x0F;
    int8_t acc = s_enc_accum + QTABLE[s_enc_state];

    /* 4 quarter-steps = 1 detent on a standard EC11. */
    if (acc >= 4 || acc <= -4) {
        ui_msg_t m = { .type = UMSG_ROT, .delta = (acc > 0) ? 1 : -1 };
        BaseType_t hpw = pdFALSE;
        xQueueSendFromISR(s_q, &m, &hpw);
        acc = 0;
        if (hpw) {
            portYIELD_FROM_ISR();
        }
    }
    s_enc_accum = acc;
}

/* ------------------------------------------------------------------ */
/* Button gestures (poll task: debounce + click/double/long)           */
/* ------------------------------------------------------------------ */

static void button_task(void *arg)
{
    (void)arg;
    int held_ms = 0;
    bool was_down = false;
    bool long_fired = false;
    int since_click_ms = -1;   /* >=0: waiting to disambiguate dbl-click */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5));
        bool down = gpio_get_level(ENC_SW) == 0;
        ui_msg_t m = {0};

        if (down) {
            held_ms += 5;
            if (held_ms >= LONG_PRESS_MS && !long_fired) {
                long_fired = true;
                since_click_ms = -1;
                m.type = UMSG_LONG;
                xQueueSend(s_q, &m, 0);
            }
        } else {
            if (was_down && !long_fired && held_ms >= 20) {
                if (since_click_ms >= 0) {
                    since_click_ms = -1;
                    m.type = UMSG_DBL;
                    xQueueSend(s_q, &m, 0);
                } else {
                    since_click_ms = 0;   /* start dbl-click window */
                }
            }
            held_ms = 0;
            long_fired = false;
        }
        was_down = down;

        if (since_click_ms >= 0 && !down) {
            since_click_ms += 5;
            if (since_click_ms > DBL_CLICK_MS) {
                since_click_ms = -1;
                m.type = UMSG_CLICK;
                xQueueSend(s_q, &m, 0);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Rendering                                                          */
/* ------------------------------------------------------------------ */

static void toast(const char *msg)
{
    snprintf(s_ui.toast, sizeof(s_ui.toast), "%s", msg);
    s_ui.toast_until_us = esp_timer_get_time() + (int64_t)TOAST_MS * 1000;
}

static void render(void)
{
    int64_t now = esp_timer_get_time();
    oled_clear();

    if (s_ui.screen == SCR_PAIRING) {
        int secs = (int)((s_ui.pair_end_us - now) / 1000000);
        if (secs < 0) {
            secs = 0;
        }
        char line[22];
        oled_text(0, 0, "PAIRING MODE", true);
        oled_text(0, 2, "Press BOOT on the", false);
        oled_text(0, 3, "dongle to pair it.", false);
        snprintf(line, sizeof(line), "%2d s left", secs);
        oled_text(0, 5, line, false);
        oled_text(0, 7, "click = cancel", false);
        oled_flush();
        return;
    }

    if (s_ui.screen == SCR_MENU) {
        char forget[22];
        const hub_pairing_t *p = store_pairing(s_ui.sel);
        snprintf(forget, sizeof(forget), "Forget %s",
                 p ? p->name : "(empty)");
        const char *items[3] = { "Pair new dongle", forget, "Cancel" };
        oled_text(0, 0, "MENU", true);
        for (int i = 0; i < 3; i++) {
            oled_text(1, 2 + i * 2, items[i], s_ui.menu_idx == i);
        }
        oled_flush();
        return;
    }

    /* --- MAIN screen ------------------------------------------------ */
    uint8_t active = link_active_slot();

    if (s_ui.cmd_mode) {
        oled_text(0, 0, " SELECT PC: 0-9      ", true);
    } else {
        char hdr[22];
        snprintf(hdr, sizeof(hdr), "espkvm            ch%d",
                 CONFIG_ESPKVM_CHANNEL);
        oled_text(0, 0, hdr, false);
    }

    if (active == KVM_SLOT_NONE) {
        oled_text(0, 3, "No dongles paired.", false);
        oled_text(0, 4, "Hold knob 3s to pair", false);
    } else {
        const hub_pairing_t *p = store_pairing(active);
        char digit[2] = { (char)('0' + active), '\0' };
        oled_text_scaled(2, 14, digit, 4);          /* big slot number  */
        oled_text_scaled(36, 18, p ? p->name : "?", 2);
        oled_text(6, 5, link_slot_online(active) ? "online" : "OFFLINE",
                  !link_slot_online(active));
    }

    if (!usb_kbd_connected()) {
        oled_text(0, 5, "no kbd!", true);
    }

    /* Toast / selection hint line */
    if (s_ui.toast[0] && now < s_ui.toast_until_us) {
        oled_text(0, 6, s_ui.toast, true);
    } else if (s_ui.sel != active && store_pairing(s_ui.sel)) {
        char hint[26];   /* "> " + 12-char name + "  (click)" + NUL */
        snprintf(hint, sizeof(hint), "> %s  (click)",
                 store_pairing(s_ui.sel)->name);
        oled_text(0, 6, hint, false);   /* oled_text clips at 21 cols */
    }

    /* Slot strip: digit per slot; inverse = active, '.'/'*' = state. */
    for (uint8_t i = 0; i < KVM_MAX_SLOTS; i++) {
        char c[2] = { (char)('0' + i), '\0' };
        uint8_t col = i * 2;
        if (!store_pairing(i)) {
            oled_text(col, 7, "-", false);
        } else {
            oled_text(col, 7, c, i == active);
            if (i == s_ui.sel && i != active) {
                /* underline the cursor */
                oled_fill_rect(col * 6, 63, 5, 1, true);
            }
            if (!link_slot_online(i)) {
                oled_pixel(col * 6 + 5, 57, true);   /* offline tick mark */
            }
        }
    }

    oled_flush();
}

/* ------------------------------------------------------------------ */
/* UI logic                                                           */
/* ------------------------------------------------------------------ */

/* Move the selection cursor to the next/previous *paired* slot (no-op
 * when nothing is paired — the loop finds no candidate). */
static void move_sel(int dir)
{
    uint8_t s = s_ui.sel;
    for (int i = 0; i < KVM_MAX_SLOTS; i++) {
        s = (uint8_t)((s + KVM_MAX_SLOTS + dir) % KVM_MAX_SLOTS);
        if (store_pairing(s)) {
            s_ui.sel = s;
            return;
        }
    }
}

static void handle_msg(const ui_msg_t *m)
{
    switch (m->type) {
    case UMSG_ROT:
        if (s_ui.screen == SCR_MAIN) {
            move_sel(m->delta);
        } else if (s_ui.screen == SCR_MENU) {
            s_ui.menu_idx = (uint8_t)((s_ui.menu_idx + 3 + m->delta) % 3);
        }
        break;

    case UMSG_CLICK:
        if (s_ui.screen == SCR_MAIN) {
            if (store_pairing(s_ui.sel)) {
                link_switch(s_ui.sel);
            }
        } else if (s_ui.screen == SCR_PAIRING) {
            link_cancel_pairing();
            s_ui.screen = SCR_MAIN;
        } else if (s_ui.screen == SCR_MENU) {
            switch (s_ui.menu_idx) {
            case 0:
                link_start_pairing();
                s_ui.screen = SCR_PAIRING;
                s_ui.pair_end_us = esp_timer_get_time()
                                 + (int64_t)PAIR_WINDOW_S * 1000000;
                break;
            case 1:
                if (store_pairing(s_ui.sel)) {
                    link_unpair(s_ui.sel);
                    toast("Slot forgotten");
                }
                s_ui.screen = SCR_MAIN;
                break;
            default:
                s_ui.screen = SCR_MAIN;
                break;
            }
        }
        break;

    case UMSG_DBL:
        if (s_ui.screen == SCR_MAIN) {
            s_ui.screen = SCR_MENU;
            s_ui.menu_idx = 0;
        }
        break;

    case UMSG_LONG:
        if (s_ui.screen != SCR_PAIRING) {
            link_start_pairing();
            s_ui.screen = SCR_PAIRING;
            s_ui.pair_end_us = esp_timer_get_time()
                             + (int64_t)PAIR_WINDOW_S * 1000000;
        }
        break;

    case UMSG_LINK:
        switch ((link_event_t)m->evt) {
        case LINK_EVT_PAIRED: {
            const hub_pairing_t *p = store_pairing(m->arg);
            char msg[22];
            snprintf(msg, sizeof(msg), "Paired %s",
                     p ? p->name : "dongle");
            toast(msg);
            s_ui.screen = SCR_MAIN;
            s_ui.sel = m->arg;
            break;
        }
        case LINK_EVT_PAIR_TIMEOUT:
            if (s_ui.screen == SCR_PAIRING) {
                toast("Pairing timeout");
                s_ui.screen = SCR_MAIN;
            }
            break;
        case LINK_EVT_PAIR_FAIL:
            toast("All 10 slots used");
            break;
        case LINK_EVT_SWITCHED:
            s_ui.sel = m->arg;
            break;
        case LINK_EVT_STATUS:
        default:
            break;
        }
        break;

    case UMSG_INPUT:
        switch ((input_event_t)m->evt) {
        case INPUT_EVT_CMD_ON:
            s_ui.cmd_mode = true;
            break;
        case INPUT_EVT_CMD_OFF:
            s_ui.cmd_mode = false;
            break;
        case INPUT_EVT_CMD_INVALID: {
            char msg[22];
            snprintf(msg, sizeof(msg), "Slot %u is empty", m->arg);
            toast(msg);
            break;
        }
        case INPUT_EVT_CMD_SWITCH:
        default:
            break;
        }
        break;

    case UMSG_TICK:
    default:
        break;
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    ui_msg_t m;
    render();
    for (;;) {
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(500)) == pdTRUE) {
            handle_msg(&m);
        }
        render();   /* also refreshes countdowns/toasts on timeout */
    }
}

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

void ui_post_link_event(link_event_t evt, uint8_t slot)
{
    ui_msg_t m = { .type = UMSG_LINK, .evt = (uint8_t)evt, .arg = slot };
    xQueueSend(s_q, &m, 0);
}

void ui_post_input_event(input_event_t evt, uint8_t arg)
{
    ui_msg_t m = { .type = UMSG_INPUT, .evt = (uint8_t)evt, .arg = arg };
    xQueueSend(s_q, &m, 0);
}

esp_err_t ui_init(void)
{
    s_q = xQueueCreate(16, sizeof(ui_msg_t));
    if (!s_q) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t active = link_active_slot();
    s_ui.sel = (active != KVM_SLOT_NONE) ? active : 0;

    /* Encoder pins: inputs with pull-ups, interrupt on both edges. */
    gpio_config_t enc = {
        .pin_bit_mask = (1ULL << ENC_A) | (1ULL << ENC_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&enc));

    gpio_config_t sw = {
        .pin_bit_mask = 1ULL << ENC_SW,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sw));

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(ENC_A, enc_isr, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(ENC_B, enc_isr, NULL));

    xTaskCreate(button_task, "ui_btn", 3072, NULL, 8, NULL);
    xTaskCreate(ui_task, "ui", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "UI up (encoder A=%d B=%d SW=%d)", ENC_A, ENC_B, ENC_SW);
    return ESP_OK;
}
