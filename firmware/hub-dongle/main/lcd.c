/*
 * espkvm hub-dongle — ST7735 80x160 status LCD (LILYGO T-Dongle-S3).
 *
 * Same panel bring-up as the plain dongle's lcd.c (plain IDF spi_master,
 * full RGB565 framebuffer pushed as one DMA transaction, landscape
 * 160x80); only the content differs — this stick is the hub, so it shows
 * the 0-9 slot bar (green = active, red = paired idle, grey = empty),
 * how to reach the phone UI, and the local USB state.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "kvm_proto.h"
#include "kvm_font.h"
#include "store.h"
#include "link.h"
#include "usb_dev.h"
#include "lcd.h"

static const char *TAG = "lcd";

#define LCD_W 160
#define LCD_H 80

/* Panel offsets for the 0.96" 80x160 ST7735S in landscape (MADCTL MV set).
 * If your panel shows a shifted image or a noise border, these two are the
 * knobs to turn (some batches use 0/24 instead of 1/26). */
#define X_OFF 1
#define Y_OFF 26

/* RGB565 (byte-swapped at store time — the panel wants big-endian) */
#define C(r5, g6, b5) ((uint16_t)(((r5) << 11) | ((g6) << 5) | (b5)))
#define COL_BLACK    C(0, 0, 0)
#define COL_WHITE    C(31, 63, 31)
#define COL_GREEN    C(4, 55, 8)
#define COL_GREEN_HI C(12, 63, 18)   /* brighter green for the active pulse */
#define COL_GREY     C(12, 24, 12)
#define COL_DIM      C(6, 12, 6)      /* pre-reveal ghost during boot */
#define COL_YELLOW   C(31, 60, 4)
#define COL_CYAN     C(6, 50, 28)
#define COL_RED      C(30, 8, 6)
#define COL_CRIMSON  C(26, 10, 10)   /* brand red, from the OctoKey emblem */

static spi_device_handle_t s_spi;
static uint16_t *s_fb;   /* LCD_W*LCD_H, DMA-capable */
static bool s_ok;

/* ------------------------------------------------------------------ */
/* Low-level panel access                                             */
/* ------------------------------------------------------------------ */

static void dc(int level)
{
    gpio_set_level(CONFIG_ESPKVM_LCD_DC, level);
}

static void cmd8(uint8_t c)
{
    spi_transaction_t t = { .length = 8, .tx_buffer = &c };
    dc(0);
    spi_device_polling_transmit(s_spi, &t);
}

static void data(const uint8_t *d, size_t len)
{
    if (len == 0) {
        return;
    }
    spi_transaction_t t = { .length = len * 8, .tx_buffer = d };
    dc(1);
    spi_device_polling_transmit(s_spi, &t);
}

static void cmd_d(uint8_t c, const uint8_t *d, size_t len)
{
    cmd8(c);
    data(d, len);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                            */
/* ------------------------------------------------------------------ */

static inline uint16_t swap16(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

static void fill(uint16_t color)
{
    uint16_t v = swap16(color);
    for (int i = 0; i < LCD_W * LCD_H; i++) {
        s_fb[i] = v;
    }
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    uint16_t v = swap16(color);
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= LCD_H) {
            continue;
        }
        for (int xx = x; xx < x + w; xx++) {
            if (xx >= 0 && xx < LCD_W) {
                s_fb[yy * LCD_W + xx] = v;
            }
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
                    fill_rect(x + gx * scale, y + gy * scale,
                              scale, scale, fg);
                }
            }
        }
        x += 6 * scale;
    }
}

/* Centered helper for a single line at `scale`. */
static void text_center(int y, const char *s, int scale, uint16_t fg)
{
    int w = (int)strlen(s) * 6 * scale - scale;
    text((LCD_W - w) / 2, y, s, scale, fg);
}

static void flush(void)
{
    uint8_t ca[4] = { 0, X_OFF, 0, X_OFF + LCD_W - 1 };
    uint8_t ra[4] = { 0, Y_OFF, 0, Y_OFF + LCD_H - 1 };
    cmd_d(0x2A, ca, 4);                 /* CASET */
    cmd_d(0x2B, ra, 4);                 /* RASET */
    cmd8(0x2C);                         /* RAMWR */

    spi_transaction_t t = {
        .length = LCD_W * LCD_H * 2 * 8,
        .tx_buffer = s_fb,
    };
    dc(1);
    spi_device_transmit(s_spi, &t);     /* DMA, blocks until done */
}

/* ------------------------------------------------------------------ */
/* Screen content                                                     */
/* ------------------------------------------------------------------ */

/* Slot bar: digits 0-9 across the top. Green = active slot, red =
 * paired but not active, grey = nothing paired there. Same scheme as
 * the T-Deck header and the phone UI's chips. */
static void draw_slotbar(uint32_t tick)
{
    const int cell = LCD_W / KVM_MAX_SLOTS;   /* 16 px */
    uint8_t active = link_active_slot();
    /* the active digit breathes between two greens so the screen is
     * never fully static */
    uint16_t active_col = (tick % 6 < 3) ? COL_GREEN : COL_GREEN_HI;
    for (int i = 0; i < KVM_MAX_SLOTS; i++) {
        uint16_t col = !store_pairing(i) ? COL_GREY
                     : i == active       ? active_col
                                         : COL_RED;
        char d[2] = { (char)('0' + i), '\0' };
        text(i * cell + (cell - 10) / 2, 3, d, 2, col);
    }
}

static void render(uint32_t tick)
{
    fill(COL_BLACK);

    if (link_pairing_active()) {
        text_center(14, "PAIRING", 3, COL_CYAN);
        char dots[4] = "";
        for (uint32_t i = 0; i < (tick / 2) % 4; i++) {
            dots[i] = '.';
        }
        text_center(44, dots, 2, COL_CYAN);
        text_center(66, "press BOOT on the new dongle", 1, COL_GREY);
        flush();
        return;
    }

    draw_slotbar(tick);

    text_center(26, "wifi " CONFIG_ESPKVM_AP_SSID, 1, COL_CYAN);
    text_center(38, "http://192.168.4.1", 1, COL_WHITE);

    uint8_t active = link_active_slot();
    const hub_pairing_t *p = (active == KVM_SLOT_NONE)
                           ? NULL : store_pairing(active);
    char line[32];
    if (p) {
        snprintf(line, sizeof(line), "> %s", p->name);
    } else {
        snprintf(line, sizeof(line), "no active slot");
    }
    text_center(54, line, 1, p ? COL_GREEN : COL_GREY);

    if (!usb_dev_mounted()) {
        text_center(68, "USB not up", 1, COL_YELLOW);
    }
    flush();
}

/* The OctoKey mark in pixels — the SVG logo boiled down to what an
 * 18x21 patch of an ST7735 can actually show: a domed head with two
 * eyes, four tentacles, and the keyboard + mouse they're holding.
 * Hand-plotted rather than a bitmap so it costs no flash beyond this. */
static void draw_octo(int x, int y, uint16_t col, int wiggle)
{
    /* dome: half-width per row, top to bottom */
    static const uint8_t half[10] = { 3, 5, 6, 7, 7, 7, 7, 7, 7, 7 };
    for (int r = 0; r < 10; r++) {
        fill_rect(x + 7 - half[r], y + r, half[r] * 2 + 1, 1, col);
    }
    /* eyes (punched back out of the dome) */
    fill_rect(x + 3, y + 5, 2, 2, COL_BLACK);
    fill_rect(x + 9, y + 5, 2, 2, COL_BLACK);
    /* four tentacles, alternating their curl with the wiggle phase */
    for (int t = 0; t < 4; t++) {
        int tx = x + 1 + t * 4;
        int dir = ((t + wiggle) & 1) ? 1 : -1;
        fill_rect(tx, y + 10, 2, 3, col);
        fill_rect(tx + dir, y + 13, 2, 3, col);
    }
    /* the gear in its grip: keyboard left, mouse right */
    fill_rect(x - 2, y + 16, 9, 5, COL_WHITE);      /* keyboard body */
    fill_rect(x - 1, y + 17, 7, 1, COL_BLACK);      /* key row */
    fill_rect(x + 1, y + 19, 3, 1, COL_BLACK);      /* space bar */
    fill_rect(x + 11, y + 16, 5, 5, COL_WHITE);     /* mouse body */
    fill_rect(x + 13, y + 17, 1, 2, COL_BLACK);     /* scroll wheel */
}

/* One-time power-on flourish, ~1.8 s: the "OctoKey" title types itself in
 * with a blinking caret, then the 0-9 digits cascade in from the left —
 * each drops into place with a white flash — while a cyan scan line
 * sweeps underneath. Purely cosmetic; the normal screen takes over after. */
#define BOOT_FRAMES 48
#define BOOT_TITLE   "OctoKey"
#define BOOT_TITLE_N 7                 /* strlen(BOOT_TITLE) */
static void draw_boot(int f)
{
    fill(COL_BLACK);
    const int cell = LCD_W / KVM_MAX_SLOTS;
    /* the mark sits left of the wordmark; both are placed from a fixed
     * left edge (each glyph is 12 px at scale 2) so the title types in
     * place instead of re-centering as it grows */
    const int tx = (LCD_W - (BOOT_TITLE_N * 12 + 22)) / 2 + 22;

    draw_octo(tx - 22, 10, COL_CRIMSON, f / 4);

    /* title: one character every 3 frames */
    char title[BOOT_TITLE_N + 1] = BOOT_TITLE;
    int shown = f / 3;
    if (shown < BOOT_TITLE_N) {
        title[shown] = '\0';
        if (f & 2) {                    /* blinking caret while typing */
            text(tx + shown * 12, 12, "_", 2, COL_WHITE);
        }
    }
    text(tx, 12, title, 2, COL_CRIMSON);

    /* digits reveal after the title starts, one every 2 frames */
    int reveal = f - 16;
    for (int i = 0; i < KVM_MAX_SLOTS; i++) {
        int x = i * cell + (cell - 10) / 2;
        char d[2] = { (char)('0' + i), '\0' };
        int appear = i * 2;
        if (reveal < appear) {
            text(x, 40, d, 2, COL_DIM);         /* ghost placeholder */
            continue;
        }
        int age = reveal - appear;
        uint16_t col = age < 3 ? COL_WHITE : COL_GREY;
        int off = age < 5 ? (5 - age) * 2 : 0;  /* slide up into place */
        text(x, 40 + off, d, 2, col);
    }

    /* cyan scan line sweeping along the bottom */
    int sx = (f * 7) % (LCD_W + 16) - 8;
    fill_rect(sx, 66, 10, 2, COL_CRIMSON);

    flush();
}

static void lcd_task(void *arg)
{
    (void)arg;

    /* Skip the flourish if we came up straight into a pairing window. */
    if (!link_pairing_active()) {
        for (int f = 0; f < BOOT_FRAMES; f++) {
            draw_boot(f);
            vTaskDelay(pdMS_TO_TICKS(38));
        }
    }

    uint32_t tick = 0;
    for (;;) {
        render(tick++);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ------------------------------------------------------------------ */
/* Init                                                               */
/* ------------------------------------------------------------------ */

esp_err_t lcd_init(void)
{
    s_fb = heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_DMA);
    if (!s_fb) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_ESPKVM_LCD_DC) |
                        (1ULL << CONFIG_ESPKVM_LCD_RST) |
                        (1ULL << CONFIG_ESPKVM_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_ESPKVM_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = CONFIG_ESPKVM_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * 2 + 16,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = CONFIG_ESPKVM_LCD_CS,
        .queue_size = 2,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev, &s_spi);
    if (err != ESP_OK) {
        return err;
    }

    /* Hardware reset */
    gpio_set_level(CONFIG_ESPKVM_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(CONFIG_ESPKVM_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    cmd8(0x11);                              /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd_d(0x3A, (const uint8_t[]){0x05}, 1); /* COLMOD: 16-bit RGB565 */
    /* MADCTL: MY|MV = landscape, BGR panel. If red/blue look swapped on
     * your unit, drop the 0x08; if the image is upside down, use 0x68. */
    cmd_d(0x36, (const uint8_t[]){0xA8}, 1);
    cmd8(0x21);                              /* INVON — these panels need it */
    cmd8(0x13);                              /* NORON */
    cmd8(0x29);                              /* DISPON */

    fill(COL_BLACK);
    flush();
    gpio_set_level(CONFIG_ESPKVM_LCD_BL, 0); /* backlight is active-low */

    s_ok = true;
    xTaskCreate(lcd_task, "lcd", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "ST7735 up (160x80 landscape)");
    return ESP_OK;
}
