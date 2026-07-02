/*
 * espkvm hub — input pipeline.
 *
 * Everything the keyboard produces flows through here exactly once:
 *
 *      USB HID task ──▶ input queue ──▶ hotkey FSM ──▶ ESP-NOW link
 *
 * The hotkey chord (default: double-tap Right-Ctrl, then a digit) is
 * intercepted *before* forwarding, so the target machine never sees it:
 *
 *   IDLE   ── RCtrl tapped alone ──▶ ARMED (window: TAP_MS)
 *   ARMED  ── RCtrl pressed again ──▶ CMD   (that press is swallowed)
 *   CMD    ── digit 0-9 ──▶ switch slot; Esc/timeout ──▶ cancel
 *
 * Subtlety: the *first* tap is forwarded as it happens (we can't see the
 * future), which is harmless — a lone Ctrl press-and-release does nothing
 * on any OS. The second press is where we diverge: it is swallowed, and
 * everything else is swallowed until command mode ends. On exit we forward
 * one empty keyboard report so the target's idea of "what is held down"
 * is resynchronised no matter what we ate.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "kvm_proto.h"
#include "store.h"
#include "link.h"
#include "input.h"

static const char *TAG = "input";

#define TAP_MS      CONFIG_ESPKVM_HOTKEY_TAP_MS
#define CMD_MS      CONFIG_ESPKVM_HOTKEY_CMD_TIMEOUT_MS

#define MOD_RCTRL   0x10   /* HID modifier bit for usage E4 (Right Ctrl) */
#define KEY_ESC     0x29

static QueueHandle_t s_q;
static input_event_cb_t s_cb;

typedef enum { HK_IDLE, HK_ARMED, HK_CMD } hk_state_t;

static struct {
    hk_state_t st;
    bool    prev_rctrl;
    bool    tap_pending;       /* RCtrl currently down as a candidate tap */
    int64_t tap_start_us;
    int64_t armed_until_us;
    int64_t cmd_until_us;
    uint8_t prev_keys[KVM_HIDP_MAX_KEYS];
} s_hk;

static void notify(input_event_t evt, uint8_t arg)
{
    if (s_cb) {
        s_cb(evt, arg);
    }
}

static void cmd_exit(void)
{
    s_hk.st = HK_IDLE;
    s_hk.tap_pending = false;
    /* Resync the target: we may have swallowed presses/releases. */
    static const uint8_t none[KVM_HIDP_MAX_KEYS] = {0};
    link_send_kbd(0, none);
    notify(INPUT_EVT_CMD_OFF, 0);
}

/* Map a HID keycode to a slot digit (top row + keypad), or -1. */
static int keycode_to_slot(uint8_t kc)
{
    if (kc >= 0x1E && kc <= 0x26) return kc - 0x1E + 1;   /* '1'..'9' */
    if (kc == 0x27)               return 0;               /* '0'      */
    if (kc >= 0x59 && kc <= 0x61) return kc - 0x59 + 1;   /* KP 1..9  */
    if (kc == 0x62)               return 0;               /* KP 0     */
    return -1;
}

/* True if `kc` appears in the previous report (i.e. not a fresh press). */
static bool was_held(uint8_t kc)
{
    for (int i = 0; i < KVM_HIDP_MAX_KEYS; i++) {
        if (s_hk.prev_keys[i] == kc) {
            return true;
        }
    }
    return false;
}

/* Returns true if the event must be swallowed (not forwarded). */
static bool hotkey_process(const kvm_hidp_out_t *ev)
{
    int64_t now = esp_timer_get_time();
    bool rctrl = (ev->mods & MOD_RCTRL) != 0;
    bool other_mods = (ev->mods & (uint8_t)~MOD_RCTRL) != 0;
    bool any_key = false;
    for (int i = 0; i < KVM_HIDP_MAX_KEYS; i++) {
        any_key = any_key || ev->keys[i] != 0;
    }
    bool other = other_mods || any_key;
    bool swallow = false;

    switch (s_hk.st) {
    case HK_CMD:
        swallow = true;
        for (int i = 0; i < KVM_HIDP_MAX_KEYS; i++) {
            uint8_t kc = ev->keys[i];
            if (kc == 0 || was_held(kc)) {
                continue;
            }
            if (kc == KEY_ESC) {
                cmd_exit();
                break;
            }
            int slot = keycode_to_slot(kc);
            if (slot >= 0) {
                if (store_pairing((uint8_t)slot)) {
                    link_switch((uint8_t)slot);
                    notify(INPUT_EVT_CMD_SWITCH, (uint8_t)slot);
                } else {
                    notify(INPUT_EVT_CMD_INVALID, (uint8_t)slot);
                }
                cmd_exit();
                break;
            }
        }
        if (s_hk.st == HK_CMD && now > s_hk.cmd_until_us) {
            cmd_exit();
        }
        break;

    case HK_ARMED:
        if (now > s_hk.armed_until_us) {
            s_hk.st = HK_IDLE;
            /* fall through to IDLE handling below via recursion-free path */
        } else if (rctrl && !other && !s_hk.prev_rctrl) {
            /* Second tap: enter command mode, eat this press. */
            s_hk.st = HK_CMD;
            s_hk.cmd_until_us = now + (int64_t)CMD_MS * 1000;
            notify(INPUT_EVT_CMD_ON, 0);
            swallow = true;
            break;
        } else if (other) {
            s_hk.st = HK_IDLE;   /* chord broken */
        }
        if (s_hk.st != HK_IDLE) {
            break;
        }
        /* fall through */

    case HK_IDLE:
    default:
        if (rctrl && !other && !s_hk.prev_rctrl) {
            s_hk.tap_pending = true;
            s_hk.tap_start_us = now;
        } else if (s_hk.tap_pending && !rctrl) {
            if (!other && now - s_hk.tap_start_us < (int64_t)TAP_MS * 1000) {
                s_hk.st = HK_ARMED;
                s_hk.armed_until_us = now + (int64_t)TAP_MS * 1000;
            }
            s_hk.tap_pending = false;
        } else if (other) {
            s_hk.tap_pending = false;
        }
        break;
    }

    s_hk.prev_rctrl = rctrl;
    memcpy(s_hk.prev_keys, ev->keys, KVM_HIDP_MAX_KEYS);
    return swallow;
}

/* ------------------------------------------------------------------ */

static void input_task(void *arg)
{
    (void)arg;
    kvm_hidp_out_t ev;

    for (;;) {
        bool got = xQueueReceive(s_q, &ev, pdMS_TO_TICKS(50)) == pdTRUE;

        if (!got) {
            /* Command-mode timeout must fire even with no keys arriving. */
            if (s_hk.st == HK_CMD &&
                esp_timer_get_time() > s_hk.cmd_until_us) {
                cmd_exit();
            } else if (s_hk.st == HK_ARMED &&
                       esp_timer_get_time() > s_hk.armed_until_us) {
                s_hk.st = HK_IDLE;
            }
            continue;
        }

        if (ev.has_kbd) {
            if (!hotkey_process(&ev)) {
                link_send_kbd(ev.mods, ev.keys);
            }
        }
        if (ev.has_mouse) {
            /* Mouse is forwarded even in command mode — swallowing clicks
             * would feel broken, and they can't collide with the chord. */
            link_send_mouse(ev.buttons, ev.dx, ev.dy, ev.wheel, ev.pan);
        }
        if (ev.has_consumer) {
            link_send_consumer(ev.consumer);
        }
    }
}

void input_submit(const kvm_hidp_out_t *ev)
{
    /* Called from the HID host task. Drop on overflow rather than block —
     * mouse move floods must never stall USB event handling. */
    xQueueSend(s_q, ev, 0);
}

esp_err_t input_init(input_event_cb_t cb)
{
    s_cb = cb;
    s_q = xQueueCreate(64, sizeof(kvm_hidp_out_t));
    if (!s_q) {
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(input_task, "kvm_input", 4096, NULL, 12, NULL);
    ESP_LOGI(TAG, "hotkey: double Right-Ctrl then 0-9 (tap window %d ms)",
             TAP_MS);
    return ESP_OK;
}
