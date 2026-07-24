/*
 * espkvm hub-tdeck — ST7789 320x240 status screen.
 *
 * Deliberately simple (status only, no on-screen menu — the trackball
 * doesn't drive a cursor precise enough to make menu widgets pleasant,
 * and every action already has a direct physical gesture; see main.c and
 * docs/PAIRING.md). One big slot digit, ACTIVE/idle, link health, and a
 * one-line legend for the key remaps this board needs (see kbd_i2c.c).
 *
 * The panel is driven through IDF's own esp_lcd ST7789 driver rather
 * than raw SPI like the T-Dongle-S3's lcd.c: a hand-rolled init sequence
 * came up garbled on real hardware (see disp_init for the two
 * board-specific requirements — SWRESET and parked chip-selects — that
 * esp_lcd/this init handle and the naive approach missed).
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

#include "kvm_proto.h"
#include "kvm_font.h"
#include "store.h"
#include "link.h"
#include "disp.h"

static const char *TAG = "disp";

#define LCD_W 320
#define LCD_H 240

/* Landscape 320x240 IPS panel, no offset on this batch. If the image is
 * shifted, X_OFF/Y_OFF are the knob (applied via esp_lcd_panel_set_gap);
 * if orientation is mirrored/upside down, adjust the swap_xy/mirror
 * calls in disp_init (LilyGO landscape = MV|MX, i.e. swap + mirror X). */
#define X_OFF 0
#define Y_OFF 0

#define C(r5, g6, b5) ((uint16_t)(((r5) << 11) | ((g6) << 5) | (b5)))
#define COL_BLACK  C(0, 0, 0)
#define COL_WHITE  C(31, 63, 31)
#define COL_GREEN  C(4, 55, 8)
#define COL_GREY   C(12, 24, 12)
#define COL_YELLOW C(31, 60, 4)
#define COL_CYAN   C(6, 50, 28)
#define COL_RED    C(30, 8, 6)

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static SemaphoreHandle_t s_lock;
static bool s_ok;

static struct {
    bool pairing;
    bool keys_open;     /* soft-key bar expanded? starts collapsed so the
                         * bar can't be hit accidentally while mousing */
    int  pair_secs;
    char toast[40];
    int64_t toast_until_us;
} s_state;

/* ------------------------------------------------------------------ */

static inline uint16_t swap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

static void fill(uint16_t color)
{
    uint16_t v = swap16(color);
    for (int i = 0; i < LCD_W * LCD_H; i++) s_fb[i] = v;
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    uint16_t v = swap16(color);
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= LCD_H) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx >= 0 && xx < LCD_W) s_fb[yy * LCD_W + xx] = v;
        }
    }
}

static void text(int x, int y, const char *s, int scale, uint16_t fg)
{
    for (; *s; s++) {
        const uint8_t *glyph = kvm_font_glyph(*s);
        for (int gx = 0; gx < 5; gx++) {
            for (int gy = 0; gy < 7; gy++) {
                if ((glyph[gx] >> gy) & 1) {
                    fill_rect(x + gx * scale, y + gy * scale, scale, scale, fg);
                }
            }
        }
        x += 6 * scale;
    }
}

static void text_center(int y, const char *s, int scale, uint16_t fg)
{
    int w = (int)strlen(s) * 6 * scale - scale;
    text((LCD_W - w) / 2, y, s, scale, fg);
}

static void flush(void)
{
    /* draw_bitmap's end coordinates are exclusive */
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb);
}

static void rect_outline(int x, int y, int w, int h, uint16_t color)
{
    fill_rect(x, y, w, 1, color);
    fill_rect(x, y + h - 1, w, 1, color);
    fill_rect(x, y, 1, h, color);
    fill_rect(x + w - 1, y, 1, h, color);
}

/* ------------------------------------------------------------------ */
/* Soft-key bar: the keys this board's physical keyboard doesn't have.
 * Drawn along the bottom of the screen; touch.c routes taps here via
 * disp_softkey_hit(). Usages 0xE0..0xE7 are HID modifiers (Win). */

typedef struct { const char *label; uint8_t usage; } softkey_t;

#define SK_ROWS 2
#define SK_COLS 6
#define SK_H    32
#define SK_TOP  (LCD_H - SK_ROWS * SK_H)

static const softkey_t SK[SK_ROWS][SK_COLS] = {
    { {"esc", 0x29}, {"tab", 0x2B}, {"win", 0xE3}, {"del", 0x4C}, {"hom", 0x4A}, {"end", 0x4D} },
    { {"<",   0x50}, {"^",   0x52}, {"v",   0x51}, {">",   0x4F}, {"pgu", 0x4B}, {"pgd", 0x4E} },
};

/* The expand/collapse tab in the bottom-right corner — always visible
 * (except while pairing) so the bar can be summoned back. */
#define HANDLE_W 48
#define HANDLE_H 16
#define HANDLE_X (LCD_W - HANDLE_W)
#define HANDLE_Y (s_state.keys_open ? SK_TOP - HANDLE_H - 1 : LCD_H - HANDLE_H)

static void draw_softkeys(void)
{
    rect_outline(HANDLE_X, HANDLE_Y, HANDLE_W - 1, HANDLE_H, COL_CYAN);
    text(HANDLE_X + 5, HANDLE_Y + 5,
         s_state.keys_open ? "hide v" : "keys ^", 1, COL_CYAN);

    if (!s_state.keys_open) return;

    for (int r = 0; r < SK_ROWS; r++) {
        for (int c = 0; c < SK_COLS; c++) {
            int x0 = c * LCD_W / SK_COLS;
            int x1 = (c + 1) * LCD_W / SK_COLS;
            int y0 = SK_TOP + r * SK_H;
            rect_outline(x0 + 1, y0 + 1, x1 - x0 - 2, SK_H - 2, COL_GREY);
            int tw = (int)strlen(SK[r][c].label) * 6 * 2 - 2;
            text(x0 + (x1 - x0 - tw) / 2, y0 + (SK_H - 14) / 2,
                 SK[r][c].label, 2, COL_WHITE);
        }
    }
}

bool disp_softkey_hit(int x, int y, uint8_t *usage)
{
    if (s_state.pairing) return false;      /* bar not drawn while pairing */

    if (x >= HANDLE_X && x < LCD_W && y >= HANDLE_Y && y < HANDLE_Y + HANDLE_H) {
        *usage = DISP_SOFTKEY_TOGGLE;
        return true;
    }
    if (!s_state.keys_open) return false;   /* collapsed: all trackpad */

    if (x < 0 || x >= LCD_W || y < SK_TOP || y >= LCD_H) return false;
    int r = (y - SK_TOP) / SK_H;
    int c = x * SK_COLS / LCD_W;
    if (r < 0 || r >= SK_ROWS || c < 0 || c >= SK_COLS) return false;
    *usage = SK[r][c].usage;
    return true;
}

void disp_softkeys_toggle(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.keys_open = !s_state.keys_open;
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ */
/* Slot bar: digits 0-9 across the top of the header. Green = active
 * slot, red = paired but not active, grey = nothing paired there. */

static void draw_slotbar(uint8_t active)
{
    const int cell = 23;                        /* 10 cells right-aligned */
    const int x0 = LCD_W - KVM_MAX_SLOTS * cell;
    for (int i = 0; i < KVM_MAX_SLOTS; i++) {
        const hub_pairing_t *p = store_pairing(i);
        uint16_t col = !p          ? COL_GREY
                     : i == active ? COL_GREEN
                                   : COL_RED;
        char d[2] = { (char)('0' + i), '\0' };
        text(x0 + i * cell + (cell - 10) / 2, 6, d, 2, col);
    }
}

static void render(void)
{
    if (!s_ok) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_state.pairing) {
        fill(COL_BLACK);
        text_center(30, "espkvm", 4, COL_CYAN);
        text_center(90, "PAIRING", 3, COL_YELLOW);
        char line[24];
        snprintf(line, sizeof(line), "%d s left", s_state.pair_secs);
        text_center(130, line, 2, COL_WHITE);
        text_center(180, "press BOOT on the dongle", 1, COL_GREY);
        goto out;
    }

    {
        uint8_t active = link_active_slot();
        fill(COL_BLACK);

        /* header */
        text(8, 6, "espkvm", 1, COL_CYAN);
        draw_slotbar(active);
        fill_rect(0, 24, LCD_W, 1, COL_GREY);

        /* status area (between header and soft keys) */
        if (active == KVM_SLOT_NONE) {
            text_center(70, "no dongles paired", 2, COL_GREY);
            text_center(102, "hold trackball 3s to pair", 1, COL_GREY);
            text_center(120, "touchpad: drag=move tap=click 2-finger=scroll", 1, COL_GREY);
            text_center(136, "ALT=ctrl  ALT+q..p=switch slot 0-9", 1, COL_GREY);
            text_center(156, "phone: wifi " CONFIG_ESPKVM_AP_SSID " -> http://192.168.4.1", 1, COL_CYAN);
        } else {
            const hub_pairing_t *p = store_pairing(active);
            char digit[2] = { (char)('0' + (active % 10)), '\0' };
            bool online = link_slot_online(active);

            text(24, 46, digit, 9, online ? COL_GREEN : COL_RED);
            text(120, 56, p ? p->name : "?", 3, COL_WHITE);
            text(120, 92, online ? "online" : "OFFLINE", 2,
                 online ? COL_GREEN : COL_RED);
            text_center(130, "ALT=ctrl  ALT+q..p=switch  SYM+ball=arrows", 1, COL_GREY);
            text_center(150, "phone: wifi " CONFIG_ESPKVM_AP_SSID " -> http://192.168.4.1", 1, COL_GREY);
        }

        draw_softkeys();
    }

out:
    /* toast banner lives at the top so it never covers the soft keys */
    if (s_state.toast[0] && esp_timer_get_time() < s_state.toast_until_us) {
        fill_rect(0, 0, LCD_W, 26, COL_CYAN);
        text_center(9, s_state.toast, 1, COL_BLACK);
    }
    flush();
    xSemaphoreGive(s_lock);
}

static void disp_task(void *arg)
{
    (void)arg;
    for (;;) {
        render();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

/* ------------------------------------------------------------------ */

esp_err_t disp_init(void)
{
    /* Force internal SRAM, not PSRAM: a PSRAM-backed buffer handed to
     * GDMA needs cache write-back coherency that isn't guaranteed for
     * one huge transfer — the DMA engine can read stale cache lines. */
    s_fb = heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_fb) return ESP_ERR_NO_MEM;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    /* The LCD shares its SPI bus with the SD card and the LoRa radio,
     * and their chip-select lines power up floating — either device can
     * sit half-selected and respond to the display's clocks. LilyGO's
     * examples park both CS lines high before the first byte goes to the
     * TFT; do the same. Backlight stays off until the first real frame
     * so the panel never shows its power-on noise. */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_ESPKVM_LCD_BL) |
                        (1ULL << CONFIG_ESPKVM_SD_CS) |
                        (1ULL << CONFIG_ESPKVM_RADIO_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;
    gpio_set_level(CONFIG_ESPKVM_LCD_BL, 0);
    gpio_set_level(CONFIG_ESPKVM_SD_CS, 1);
    gpio_set_level(CONFIG_ESPKVM_RADIO_CS, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_ESPKVM_LCD_MOSI,
        .miso_io_num = CONFIG_ESPKVM_LCD_MISO,
        .sclk_io_num = CONFIG_ESPKVM_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * 2 + 16,
    };
    err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = CONFIG_ESPKVM_LCD_CS,
        .dc_gpio_num = CONFIG_ESPKVM_LCD_DC,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 4,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io);
    if (err != ESP_OK) return err;

    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = -1,          /* no reset pin wired -> SWRESET */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(s_io, &dev_cfg, &s_panel);
    if (err != ESP_OK) return err;

    /* With no reset GPIO configured this issues a software reset, which
     * the panel needs before init — there's no other way to get it out
     * of whatever state power-up (or a previous crashed image) left its
     * registers in on this board. */
    err = esp_lcd_panel_reset(s_panel);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_init(s_panel);   /* SLPOUT, COLMOD 16bpp, MADCTL */
    if (err != ESP_OK) return err;

    /* Panel-batch tuning from LilyGO's own T-Deck TFT_eSPI
     * ST7789_Init.h (JLX240 glass): frame-rate porch, gate/VCOM/power
     * voltages and gamma tables. */
    esp_lcd_panel_io_tx_param(s_io, 0xB2, (uint8_t[]){0x0C, 0x0C, 0x00, 0x33, 0x33}, 5);
    esp_lcd_panel_io_tx_param(s_io, 0xB7, (uint8_t[]){0x75}, 1);
    esp_lcd_panel_io_tx_param(s_io, 0xBB, (uint8_t[]){0x1A}, 1);
    esp_lcd_panel_io_tx_param(s_io, 0xC0, (uint8_t[]){0x2C}, 1);
    esp_lcd_panel_io_tx_param(s_io, 0xC2, (uint8_t[]){0x01}, 1);
    esp_lcd_panel_io_tx_param(s_io, 0xC3, (uint8_t[]){0x13}, 1);
    esp_lcd_panel_io_tx_param(s_io, 0xC4, (uint8_t[]){0x20}, 1);
    esp_lcd_panel_io_tx_param(s_io, 0xC6, (uint8_t[]){0x0F}, 1);
    esp_lcd_panel_io_tx_param(s_io, 0xD0, (uint8_t[]){0xA4, 0xA1}, 2);
    esp_lcd_panel_io_tx_param(s_io, 0xE0, (uint8_t[]){0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38,
                                                      0x44, 0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30}, 14);
    esp_lcd_panel_io_tx_param(s_io, 0xE1, (uint8_t[]){0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37,
                                                      0x44, 0x4D, 0x38, 0x15, 0x16, 0x2C, 0x3E}, 14);

    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_swap_xy(s_panel, true);       /* MADCTL MV — landscape   */
    esp_lcd_panel_mirror(s_panel, true, false); /* MADCTL MX — scan origin */
    esp_lcd_panel_set_gap(s_panel, X_OFF, Y_OFF);

    /* Let the charge pumps settle on the new voltage config before the
     * panel starts scanning out, then again before showing it. */
    vTaskDelay(pdMS_TO_TICKS(120));
    err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(120));

    fill(COL_BLACK);
    flush();
    gpio_set_level(CONFIG_ESPKVM_LCD_BL, 1);

    s_ok = true;
    xTaskCreate(disp_task, "disp", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "ST7789 up (320x240, esp_lcd)");
    return ESP_OK;
}

void disp_update(void) { /* render() runs on its own 300ms tick */ }

void disp_toast(const char *text_)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_state.toast, sizeof(s_state.toast), "%s", text_);
    s_state.toast_until_us = esp_timer_get_time() + 2500000;
    xSemaphoreGive(s_lock);
}

void disp_set_pairing(bool active, int seconds_left)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.pairing = active;
    s_state.pair_secs = seconds_left;
    xSemaphoreGive(s_lock);
}
