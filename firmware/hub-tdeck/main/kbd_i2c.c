/*
 * espkvm hub-tdeck — I2C keyboard driver + hotkey chord.
 *
 * The keyboard is a second ESP32-C3 on the board, scanning a 5x7 matrix
 * and exposed as an I2C slave at address 0x55 (LilyGO's official
 * Keyboard_ESP32C3 / Keyboard_T_Deck_Master examples). Its *default*
 * mode hands back one pre-decoded ASCII byte per read — Shift and the
 * Symbol layer already applied, with no way to tell which physical
 * modifier produced it. LilyGO added a "raw mode" command (0x03) in a
 * 2025 firmware update that instead returns the live 5-byte column
 * bitmap (7 rows each) every read — a real press/hold/release matrix
 * snapshot, exactly what a keyboard driver needs. We use raw mode.
 *
 * Physical layout (col, row), ported from LilyGO's Keyboard_ESP32C3.ino
 * so our decode matches the silkscreen exactly:
 *
 *   col0: q  w  [Sym] a  [Alt] Sp [Mic]
 *   col1: e  s  d     p  x     z  [LShift]
 *   col2: r  g  t     [RShift] v c  f
 *   col3: u  h  y     [Enter]  b  n  j
 *   col4: o  l  i     [Backsp] $  m  k
 *   symbol layer: # 1 . * . . 0 / 2 4 5 @ 8 7 . / 3 / ( . ? 9 6
 *                 _ : ) . ! , ; / + " - . . . '
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "kvm_proto.h"
#include "store.h"
#include "link.h"
#include "disp.h"
#include "kbd_i2c.h"

static const char *TAG = "kbd_i2c";

#define KB_ADDR          0x55
#define KB_MODE_RAW_CMD  0x03
#define COLS 5
#define ROWS 7
#define POLL_MS 15

#define MOD_LCTRL  0x01
#define MOD_LSHIFT 0x02

/* Special (col,row) positions that are never forwarded as characters. */
#define POS_SYMBOL  0, 2
#define POS_ALT     0, 4
#define POS_MIC     0, 6
#define POS_LSHIFT  1, 6
#define POS_RSHIFT  2, 3
#define POS_ENTER   3, 3
#define POS_BKSP    4, 3
#define POS_SPACE   0, 5

/* keyboard[col][row]: plain layer. 0 = no character (special key). */
static const char LAYER_PLAIN[COLS][ROWS] = {
    {'q', 'w', 0,   'a', 0,   ' ', 0  },
    {'e', 's', 'd', 'p', 'x', 'z', 0  },
    {'r', 'g', 't', 0,   'v', 'c', 'f'},
    {'u', 'h', 'y', 0,   'b', 'n', 'j'},
    {'o', 'l', 'i', 0,   '$', 'm', 'k'},
};

/* keyboard_symbol[col][row]: Symbol-key layer (numbers/punctuation). */
static const char LAYER_SYMBOL[COLS][ROWS] = {
    {'#', '1', 0,   '*', 0,   0,   '0'},
    {'2', '4', '5', '@', '8', '7', 0  },
    {'3', '/', '(', 0,   '?', '9', '6'},
    {'_', ':', ')', 0,   '!', ',', ';'},
    {'+', '"', '-', 0,   0,   '.', '\''},
};

/* Digits reachable via the symbol layer, needed verbatim during the
 * hotkey chord regardless of whether Symbol happens to be held —
 * mirrors input.c's keycode_to_slot() on the USB-host hub variants. */
static int8_t digit_at(uint8_t col, uint8_t row)
{
    static const struct { uint8_t col, row; int8_t digit; } MAP[] = {
        {0, 6, 0}, {0, 1, 1}, {1, 0, 2}, {2, 0, 3}, {1, 1, 4},
        {1, 2, 5}, {2, 6, 6}, {1, 5, 7}, {1, 4, 8}, {2, 5, 9},
    };
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        if (MAP[i].col == col && MAP[i].row == row) {
            return MAP[i].digit;
        }
    }
    return -1;
}

/* ALT + top letter row = direct slot switch (q w e r t y u i o p ->
 * slots 0-9, matching the digits on the dongle/hub screens). Only fires
 * for slots that actually have a dongle paired — ALT+letter on an empty
 * slot falls through as a normal Ctrl+letter so browser shortcuts on
 * unused letters keep working. (col,row) per LAYER_PLAIN. */
static int8_t slot_at(uint8_t col, uint8_t row)
{
    static const struct { uint8_t col, row; int8_t slot; } MAP[] = {
        {0, 0, 0},  /* q */  {0, 1, 1},  /* w */  {1, 0, 2},  /* e */
        {2, 0, 3},  /* r */  {2, 2, 4},  /* t */  {3, 2, 5},  /* y */
        {3, 0, 6},  /* u */  {4, 2, 7},  /* i */  {4, 0, 8},  /* o */
        {1, 3, 9},  /* p */
    };
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        if (MAP[i].col == col && MAP[i].row == row) {
            return MAP[i].slot;
        }
    }
    return -1;
}

/* Standard US HID keyboard-page usage for a resolved ASCII character.
 * Returns 0 (no usage) for anything we don't have a mapping for. */
static uint8_t ascii_to_hid(char c, bool *needs_shift)
{
    *needs_shift = false;
    if (c >= 'a' && c <= 'z') return (uint8_t)(0x04 + (c - 'a'));
    if (c >= 'A' && c <= 'Z') { *needs_shift = true; return (uint8_t)(0x04 + (c - 'A')); }
    if (c >= '1' && c <= '9') return (uint8_t)(0x1E + (c - '1'));
    switch (c) {
    case '0': return 0x27;
    case ' ': return 0x2C;
    case '\r': return 0x28;             /* Enter               */
    case '\b': return 0x2A;             /* Backspace            */
    case '/': return 0x38;
    case ',': return 0x36;
    case ';': return 0x33;
    case '-': return 0x2D;
    case '.': return 0x37;
    case '\'': return 0x34;
    case '#': *needs_shift = true; return 0x20;   /* Shift+3 */
    case '*': *needs_shift = true; return 0x25;   /* Shift+8 */
    case '@': *needs_shift = true; return 0x1F;   /* Shift+2 */
    case '(': *needs_shift = true; return 0x26;   /* Shift+9 */
    case '_': *needs_shift = true; return 0x2D;   /* Shift+- */
    case ':': *needs_shift = true; return 0x33;   /* Shift+; */
    case ')': *needs_shift = true; return 0x27;   /* Shift+0 */
    case '!': *needs_shift = true; return 0x1E;   /* Shift+1 */
    case '+': *needs_shift = true; return 0x2E;   /* Shift+= */
    case '"': *needs_shift = true; return 0x34;   /* Shift+' */
    case '?': *needs_shift = true; return 0x38;   /* Shift+/ */
    case '$': *needs_shift = true; return 0x21;   /* Shift+4 */
    default: return 0;
    }
}

/* ------------------------------------------------------------------ */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_ok;
static volatile uint8_t s_cur_mods;

i2c_master_bus_handle_t kbd_i2c_bus(void) { return s_bus; }

uint8_t kbd_i2c_mods(void) { return s_cur_mods; }

static bool bit_at(const uint8_t raw[COLS], uint8_t col, uint8_t row)
{
    return (raw[col] >> row) & 1;
}

/* ------------------------------------------------------------------ */
/* Hotkey chord: double-tap ALT (bare, no other key pressed meanwhile),
 * then a digit-bearing key -> switch slots. Mirrors input.c's FSM. */

typedef enum { HK_IDLE, HK_ARMED, HK_CMD } hk_state_t;

static struct {
    hk_state_t st;
    bool    alt_prev;
    bool    other_during_hold;
    int64_t alt_down_us;
    int64_t armed_until_us;
    int64_t cmd_until_us;
} s_hk;

#define TAP_MS      CONFIG_ESPKVM_HOTKEY_TAP_MS
#define CMD_MS      CONFIG_ESPKVM_HOTKEY_CMD_TIMEOUT_MS

static void send_release_all(void)
{
    static const uint8_t none[6] = {0};
    link_send_kbd(0, none);
}

/* Call on every transition of the physical ALT key. Handles the
 * double-tap arm/fire logic; may put the FSM into HK_CMD (in which
 * case this same ALT press must be swallowed, not sent as Ctrl-down). */
static void hk_on_alt_transition(bool alt_down)
{
    int64_t now = esp_timer_get_time();

    if (s_hk.st == HK_ARMED && now > s_hk.armed_until_us) {
        s_hk.st = HK_IDLE;
    }

    if (alt_down) {
        if (s_hk.st == HK_ARMED) {
            s_hk.st = HK_CMD;
            s_hk.cmd_until_us = now + (int64_t)CMD_MS * 1000;
            disp_toast("select slot 0-9");
            return;
        }
        s_hk.alt_down_us = now;
        s_hk.other_during_hold = false;
    } else {
        bool bare_tap = !s_hk.other_during_hold &&
                        (now - s_hk.alt_down_us) < (int64_t)TAP_MS * 1000;
        s_hk.st = (bare_tap && s_hk.st == HK_IDLE) ? HK_ARMED : HK_IDLE;
        if (s_hk.st == HK_ARMED) {
            s_hk.armed_until_us = now + (int64_t)TAP_MS * 1000;
        }
    }
}

/* Call once per poll tick with whichever (col,row) character key was
 * newly pressed this tick (col=row=0xFF if none). Only acts while in
 * HK_CMD; returns true if this tick's report must be fully swallowed
 * (true whenever HK_CMD is active, consumed a digit or not). */
static bool hk_consume_cmd(uint8_t new_col, uint8_t new_row)
{
    if (s_hk.st != HK_CMD) {
        return false;
    }
    int64_t now = esp_timer_get_time();

    if (new_col != 0xFF) {
        int8_t d = digit_at(new_col, new_row);
        if (d >= 0 && store_pairing((uint8_t)d)) {
            link_switch((uint8_t)d);
            disp_toast("switched");
        } else if (d >= 0) {
            disp_toast("empty slot");
        } else {
            disp_toast("cancelled");
        }
        s_hk.st = HK_IDLE;
        send_release_all();
    } else if (now > s_hk.cmd_until_us) {
        s_hk.st = HK_IDLE;
        disp_toast("cancelled");
        send_release_all();
    }
    return true;
}

/* ------------------------------------------------------------------ */

static void poll_task(void *arg)
{
    (void)arg;
    uint8_t raw[COLS] = {0}, prev[COLS] = {0};
    /* Keys consumed by an ALT+letter slot switch stay masked out of the
     * report until physically released, so the letter never leaks to
     * the newly-selected computer as a stray Ctrl+letter. */
    uint8_t swallow[COLS] = {0};

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        if (i2c_master_receive(s_dev, raw, COLS, 50) != ESP_OK) {
            continue;   /* transient bus hiccup — try again next tick */
        }
        bool changed = memcmp(raw, prev, COLS) != 0;
        if (!changed && s_hk.st != HK_CMD) {
            continue;   /* nothing held, nothing changed, not mid-chord */
        }

        bool alt_down   = bit_at(raw, POS_ALT);
        bool alt_prev   = bit_at(prev, POS_ALT);
        bool sym_down   = bit_at(raw, POS_SYMBOL);
        bool shift_down = bit_at(raw, POS_LSHIFT) || bit_at(raw, POS_RSHIFT);

        if (alt_down != alt_prev) {
            hk_on_alt_transition(alt_down);
        }

        /* Find at most one newly-pressed character key this tick — the
         * chord only ever wants the first one, and a 5x7 membrane
         * matrix realistically produces one intentional keystroke at a
         * time anyway. */
        uint8_t new_col = 0xFF, new_row = 0xFF;
        uint8_t keys[6] = {0};
        uint8_t nkeys = 0;
        /* Physical Shift is forwarded as a live modifier (not just used
         * to uppercase letters locally) so Shift+Tab / Shift+arrows /
         * Shift+click work through the soft keys and touchpad. */
        uint8_t mods = (alt_down ? MOD_LCTRL : 0) | (shift_down ? MOD_LSHIFT : 0);
        s_cur_mods = mods;

        for (uint8_t col = 0; col < COLS; col++) {
            for (uint8_t row = 0; row < ROWS; row++) {
                bool down = bit_at(raw, col, row);
                if ((col == 0 && row == 2) || (col == 0 && row == 4) ||
                    (col == 0 && row == 6) || (col == 1 && row == 6) ||
                    (col == 2 && row == 3)) {
                    continue;   /* Symbol / Alt / Mic / Shift x2 */
                }
                if ((swallow[col] >> row) & 1) {
                    if (!down) swallow[col] &= (uint8_t)~(1u << row);
                    continue;   /* spent on a slot switch, wait for release */
                }
                if (down && !bit_at(prev, col, row) && new_col == 0xFF) {
                    new_col = col;
                    new_row = row;
                }
                if (!down || nkeys >= 6) continue;

                char c = sym_down ? LAYER_SYMBOL[col][row] : LAYER_PLAIN[col][row];
                if (col == 4 && row == 3) c = '\b';        /* Backspace */
                if (col == 3 && row == 3) c = '\r';        /* Enter     */
                if (c == 0) continue;
                if (shift_down && c >= 'a' && c <= 'z') c = (char)toupper((unsigned char)c);

                bool need_shift = false;
                uint8_t usage = ascii_to_hid(c, &need_shift);
                if (usage == 0) continue;
                if (need_shift) mods |= MOD_LSHIFT;
                keys[nkeys++] = usage;
            }
        }

        if (alt_down && new_col != 0xFF) {
            s_hk.other_during_hold = true;   /* Alt+key: chord or Ctrl+key */

            int8_t sl = slot_at(new_col, new_row);
            if (sl >= 0 && store_pairing((uint8_t)sl)) {
                char msg[16];
                snprintf(msg, sizeof(msg), "-> slot %d", sl);
                link_switch((uint8_t)sl);    /* releases all keys on the
                                              * old target internally */
                disp_toast(msg);
                swallow[new_col] |= (uint8_t)(1u << new_row);
                memcpy(prev, raw, COLS);
                continue;                    /* swallow this report */
            }
        }

        if (!hk_consume_cmd(new_col, new_row)) {
            link_send_kbd(mods, keys);
        }

        memcpy(prev, raw, COLS);
    }
}

/* ------------------------------------------------------------------ */

bool kbd_i2c_symbol_held(void)
{
    /* Cheap enough to re-read here rather than cache: trackball.c polls
     * at a similar cadence and correctness matters more than one extra
     * I2C transaction. */
    uint8_t raw[COLS];
    if (i2c_master_receive(s_dev, raw, COLS, 20) != ESP_OK) {
        return false;
    }
    return bit_at(raw, POS_SYMBOL);
}

esp_err_t kbd_i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_ESPKVM_I2C_SDA,
        .scl_io_num = CONFIG_ESPKVM_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) return err;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = KB_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) return err;

    /* Give the C3 co-processor time to boot (LilyGO's own example waits
     * 500ms after driving BOARD_POWERON high — done in main.c before we
     * get here), then switch it into raw (live matrix) mode. */
    uint8_t raw_cmd = KB_MODE_RAW_CMD;
    err = i2c_master_transmit(s_dev, &raw_cmd, 1, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "keyboard co-processor not responding at 0x%02X", KB_ADDR);
        return err;
    }

    s_ok = true;
    xTaskCreate(poll_task, "kbd_i2c", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "I2C keyboard up (raw matrix mode)");
    return ESP_OK;
}
