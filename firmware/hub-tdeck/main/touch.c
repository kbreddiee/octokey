/*
 * espkvm hub-tdeck — the LCD's GT911 capacitive overlay as a trackpad.
 *
 * The trackball's mechanical detents yield a handful of coarse pulses
 * per swipe — that can't be made smooth by scaling. The touch overlay
 * reports real coordinates at ~100 Hz, so it is the primary pointer:
 *
 *   one finger drag      -> cursor movement
 *   quick tap            -> left click
 *   two-finger tap       -> right click
 *   two-finger drag      -> scroll wheel
 *
 * Gesture classification is a C port of hub-air's web touchpad logic
 * (firmware/hub-air/main/web/index.html): finger count for a gesture is
 * decided by the maximum simultaneous contacts seen during it, a "tap"
 * is a short contact that never strayed from its start point.
 *
 * The GT911 shares kbd_i2c.c's I2C bus (keyboard C3 at 0x55; the GT911
 * lands at 0x5D or 0x14 depending on how its INT pin was strapped at
 * power-up, so both are probed). Polled — the INT line is left unused.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "kvm_proto.h"
#include "link.h"
#include "kbd_i2c.h"
#include "disp.h"
#include "touch.h"

static const char *TAG = "touch";

#define GT911_ADDR_A    0x5D
#define GT911_ADDR_B    0x14
#define REG_PRODUCT_ID  0x8140      /* 4 bytes, ASCII "911"           */
#define REG_STATUS      0x814E      /* bit7 = data ready, low nibble = n */
#define REG_POINT1      0x814F      /* 8 bytes/point: id, x16, y16, sz16, rsvd */

#define POLL_MS         15
#define TAP_MAX_MS      250         /* contact shorter than this = tap  */
#define TAP_MAX_MOVE    12          /* panel px of drift a tap may have */
#define WHEEL_DIV       30          /* panel px per scroll notch        */
#define RELEASE_TO_MS   150         /* missed-release watchdog          */
#define SPEED           CONFIG_ESPKVM_TOUCH_SPEED

/* The overlay reports in the panel's native portrait frame (240x320);
 * the UI runs landscape (MADCTL MV|MX, disp.c) — rotate deltas and
 * absolute positions the same way (verified on real hardware). If a
 * different panel batch comes out rotated/mirrored, these are the knob. */
#define SCREEN_DX(pdx, pdy) (pdy)
#define SCREEN_DY(pdx, pdy) (-(pdx))
#define SCREEN_X(px, py)    (py)
#define SCREEN_Y(px, py)    (239 - (px))

static i2c_master_dev_handle_t s_dev;

static esp_err_t reg_read(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t a[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_transmit_receive(s_dev, a, 2, buf, len, 50);
}

static esp_err_t reg_write8(uint16_t reg, uint8_t v)
{
    uint8_t a[3] = { reg >> 8, reg & 0xFF, v };
    return i2c_master_transmit(s_dev, a, 3, 50);
}

static void tap_buttons(uint8_t bits)
{
    link_send_mouse(bits, 0, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(40));
    link_send_mouse(0, 0, 0, 0, 0);
}

/* Tap a soft key: merge in whatever modifiers are physically held so
 * Shift+Tab, Ctrl+arrows etc. compose naturally. Usages 0xE0..0xE7 are
 * themselves modifiers (Win) and are sent as a bare modifier tap. */
static void tap_key(uint8_t usage)
{
    static const uint8_t none[6] = {0};
    uint8_t mods = kbd_i2c_mods();

    if (usage >= 0xE0 && usage <= 0xE7) {
        link_send_kbd(mods | (uint8_t)(1u << (usage - 0xE0)), none);
    } else {
        uint8_t keys[6] = { usage, 0, 0, 0, 0, 0 };
        link_send_kbd(mods, keys);
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    link_send_kbd(kbd_i2c_mods(), none);
}

/* ------------------------------------------------------------------ */

static void touch_task(void *arg)
{
    (void)arg;
    bool touching = false, moved = false;
    int  max_fingers = 0;
    int  sx = 0, sy = 0;                /* gesture start (panel frame) */
    int  lx = 0, ly = 0;                /* previous sample             */
    int  wheel_acc = 0;
    uint8_t sk_usage = 0;               /* nonzero: gesture began on a soft key */
    int64_t t_start = 0, t_seen = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        int64_t now = esp_timer_get_time();

        uint8_t st = 0;
        if (reg_read(REG_STATUS, &st, 1) != ESP_OK) continue;

        int n = -1;                     /* -1 = no fresh report */
        int x = lx, y = ly;
        if (st & 0x80) {
            n = st & 0x0F;
            if (n > 0) {
                uint8_t d[8];
                if (reg_read(REG_POINT1, d, sizeof(d)) == ESP_OK) {
                    x = d[1] | (d[2] << 8);
                    y = d[3] | (d[4] << 8);
                } else {
                    n = -1;
                }
            }
            reg_write8(REG_STATUS, 0);  /* ack, or the buffer stalls */
        }

        if (n < 0) {
            /* No fresh data. While touched the GT911 reports every scan
             * cycle, so a long silence means we lost the release report. */
            if (touching && (now - t_seen) > RELEASE_TO_MS * 1000) n = 0;
            else continue;
        } else {
            t_seen = now;
        }

        if (n == 0) {                   /* ---- release ---- */
            if (touching) {
                touching = false;
                if (sk_usage) {
                    /* soft-key gesture: fire on release unless the
                     * finger wandered off the bar (= cancelled) */
                    if (!moved && (now - t_start) < 600 * 1000) {
                        tap_key(sk_usage);
                    }
                } else if (!moved && (now - t_start) < (int64_t)TAP_MAX_MS * 1000) {
                    tap_buttons(max_fingers >= 2 ? 0x02 : 0x01);
                }
            }
            continue;
        }

        if (!touching) {                /* ---- first contact ---- */
            touching = true;
            moved = false;
            max_fingers = n;
            sx = lx = x;
            sy = ly = y;
            wheel_acc = 0;
            t_start = now;
            sk_usage = 0;
            disp_softkey_hit(SCREEN_X(x, y), SCREEN_Y(x, y), &sk_usage);
            continue;
        }

        /* ---- ongoing gesture ---- */
        if (n > max_fingers) max_fingers = n;
        int pdx = x - lx, pdy = y - ly;
        lx = x;
        ly = y;
        if (abs(x - sx) > TAP_MAX_MOVE || abs(y - sy) > TAP_MAX_MOVE) {
            moved = true;
        }

        if (sk_usage) continue;         /* key press in progress — not a pointer gesture */

        int dx = SCREEN_DX(pdx, pdy);
        int dy = SCREEN_DY(pdx, pdy);

        if (max_fingers >= 2) {         /* two fingers: scroll, not move */
            wheel_acc += dy;
            int notches = wheel_acc / WHEEL_DIV;
            if (notches) {
                wheel_acc -= notches * WHEEL_DIV;
                link_send_mouse(0, 0, 0, (int8_t)-notches, 0);
            }
        } else if (dx || dy) {
            link_send_mouse(0, (int16_t)(dx * SPEED), (int16_t)(dy * SPEED), 0, 0);
        }
    }
}

/* ------------------------------------------------------------------ */

esp_err_t touch_init(void)
{
    i2c_master_bus_handle_t bus = kbd_i2c_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;

    static const uint8_t ADDRS[] = { GT911_ADDR_A, GT911_ADDR_B };
    for (size_t i = 0; i < sizeof(ADDRS); i++) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ADDRS[i],
            .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) continue;

        uint8_t id[4] = {0};
        if (reg_read(REG_PRODUCT_ID, id, sizeof(id)) == ESP_OK &&
            id[0] == '9' && id[1] == '1' && id[2] == '1') {
            reg_write8(REG_STATUS, 0);
            xTaskCreate(touch_task, "touch", 4096, NULL, 9, NULL);
            ESP_LOGI(TAG, "GT911 touch up at 0x%02X", ADDRS[i]);
            return ESP_OK;
        }
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    ESP_LOGW(TAG, "GT911 not found (0x%02X/0x%02X)", GT911_ADDR_A, GT911_ADDR_B);
    return ESP_ERR_NOT_FOUND;
}
