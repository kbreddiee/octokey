/*
 * espkvm hub — T-Embed display backend (ST7789 320x170 over SPI).
 *
 * Implements the same API as oled.c (see oled.h): the UI keeps rendering
 * into a 128x64 monochrome framebuffer exactly as it does for the SSD1306,
 * and flush() paints it 2x-scaled and centered onto the color panel
 * (256x128 used area inside 320x170, warm white on black). One rendering
 * path, two very different displays.
 *
 * Panel notes (LILYGO T-Embed, official pin_config.h):
 *   CS=10 DC=13 CLK=12 MOSI=11 RST=9 BL=15 (active high), 170x320 ST7789
 *   used landscape => MADCTL MV|MX, x-offset 0, y-offset 35, INVON needed.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "kvm_font.h"
#include "oled.h"

static const char *TAG = "st7789";

#define PANEL_W 320
#define PANEL_H 170
#define X_OFF   0
#define Y_OFF   35
#define SCALE   2
#define ORG_X   ((PANEL_W - OLED_W * SCALE) / 2)   /* 32 */
#define ORG_Y   ((PANEL_H - OLED_H * SCALE) / 2)   /* 21 */

/* RGB565, byte-swapped at store time (panel wants big-endian) */
#define COL_FG 0xFFFF                   /* white     */
#define COL_BG 0x0000                   /* black     */

static spi_device_handle_t s_spi;
static uint8_t s_fb[OLED_W * OLED_H / 8];   /* mono, page-major (as SSD1306) */
static uint16_t *s_pix;                     /* PANEL_W*PANEL_H, DMA-capable  */
static bool s_ok;

/* ------------------------------------------------------------------ */
/* Panel access                                                       */
/* ------------------------------------------------------------------ */

static void cmd8(uint8_t c)
{
    spi_transaction_t t = { .length = 8, .tx_buffer = &c };
    gpio_set_level(CONFIG_ESPKVM_LCD_DC, 0);
    spi_device_polling_transmit(s_spi, &t);
}

static void data(const uint8_t *d, size_t len)
{
    spi_transaction_t t = { .length = len * 8, .tx_buffer = d };
    gpio_set_level(CONFIG_ESPKVM_LCD_DC, 1);
    spi_device_polling_transmit(s_spi, &t);
}

static void cmd_d(uint8_t c, const uint8_t *d, size_t len)
{
    cmd8(c);
    data(d, len);
}

/* ------------------------------------------------------------------ */
/* oled.h API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t oled_init(void)
{
    s_pix = heap_caps_malloc(PANEL_W * PANEL_H * 2, MALLOC_CAP_DMA);
    if (!s_pix) {
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
        .max_transfer_sz = PANEL_W * PANEL_H * 2 + 16,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = CONFIG_ESPKVM_LCD_CS,
        .queue_size = 2,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev, &s_spi);
    if (err != ESP_OK) {
        return err;
    }

    gpio_set_level(CONFIG_ESPKVM_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(CONFIG_ESPKVM_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    cmd8(0x11);                               /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd_d(0x3A, (const uint8_t[]){0x05}, 1);  /* RGB565 */
    /* Landscape 320x170. If the image is upside down on your unit, use
     * 0xA0 instead of 0x60. */
    cmd_d(0x36, (const uint8_t[]){0x60}, 1);
    cmd8(0x21);                               /* INVON (IPS panel) */
    cmd8(0x13);                               /* NORON */
    cmd8(0x29);                               /* DISPON */

    s_ok = true;
    oled_clear();
    oled_flush();
    gpio_set_level(CONFIG_ESPKVM_LCD_BL, 1);  /* backlight on (active high) */

    ESP_LOGI(TAG, "T-Embed ST7789 up (320x170, UI scaled 2x)");
    return ESP_OK;
}

void oled_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void oled_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) {
        return;
    }
    uint16_t idx = (y / 8) * OLED_W + x;
    uint8_t bit = 1u << (y % 8);
    if (on) {
        s_fb[idx] |= bit;
    } else {
        s_fb[idx] &= ~bit;
    }
}

void oled_fill_rect(int x, int y, int w, int h, bool on)
{
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            oled_pixel(xx, yy, on);
        }
    }
}

void oled_text(uint8_t col, uint8_t row, const char *s, bool inv)
{
    if (row > 7) {
        return;
    }
    for (; *s && col < 21; s++, col++) {
        const uint8_t *glyph = kvm_font_glyph(*s);
        uint16_t base = row * OLED_W + col * 6;
        for (int i = 0; i < 5; i++) {
            s_fb[base + i] = inv ? (uint8_t)~glyph[i] : glyph[i];
        }
        s_fb[base + 5] = inv ? 0xFF : 0x00;
    }
}

void oled_text_scaled(int x, int y, const char *s, uint8_t scale)
{
    if (scale == 0) {
        scale = 1;
    }
    for (; *s; s++) {
        const uint8_t *glyph = kvm_font_glyph(*s);
        for (int gx = 0; gx < 5; gx++) {
            for (int gy = 0; gy < 8; gy++) {
                if ((glyph[gx] >> gy) & 1) {
                    oled_fill_rect(x + gx * scale, y + gy * scale,
                                   scale, scale, true);
                }
            }
        }
        x += 6 * scale;
    }
}

void oled_flush(void)
{
    if (!s_ok) {
        return;
    }

    /* Expand the mono framebuffer to RGB565, 2x, centered. */
    memset(s_pix, 0, PANEL_W * PANEL_H * 2);
    for (int y = 0; y < OLED_H; y++) {
        for (int x = 0; x < OLED_W; x++) {
            if (!((s_fb[(y / 8) * OLED_W + x] >> (y % 8)) & 1)) {
                continue;
            }
            int px = ORG_X + x * SCALE;
            int py = ORG_Y + y * SCALE;
            for (int dy = 0; dy < SCALE; dy++) {
                uint16_t *row = &s_pix[(py + dy) * PANEL_W + px];
                for (int dx = 0; dx < SCALE; dx++) {
                    row[dx] = COL_FG;
                }
            }
        }
    }

    uint8_t ca[4] = { X_OFF >> 8, X_OFF & 0xFF,
                      (X_OFF + PANEL_W - 1) >> 8, (X_OFF + PANEL_W - 1) & 0xFF };
    uint8_t ra[4] = { Y_OFF >> 8, Y_OFF & 0xFF,
                      (Y_OFF + PANEL_H - 1) >> 8, (Y_OFF + PANEL_H - 1) & 0xFF };
    cmd_d(0x2A, ca, 4);
    cmd_d(0x2B, ra, 4);
    cmd8(0x2C);

    spi_transaction_t t = {
        .length = PANEL_W * PANEL_H * 2 * 8,
        .tx_buffer = s_pix,
    };
    gpio_set_level(CONFIG_ESPKVM_LCD_DC, 1);
    spi_device_transmit(s_spi, &t);
}
