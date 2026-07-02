/*
 * espkvm dongle — ST7735 80x160 status LCD (LILYGO T-Dongle-S3).
 *
 * Plain IDF spi_master driver, no component dependencies. The panel is
 * used in landscape (160x80). Rendering goes to a full RGB565 framebuffer
 * in DMA memory and is pushed as one SPI transaction, ~5 times a second.
 *
 * Screen states:
 *   not paired : "NOT PAIRED / press button"
 *   pairing    : "PAIRING..."
 *   paired     : huge slot digit + ACTIVE (green) / idle (grey),
 *                plus warnings when the hub is silent or USB is down.
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
#define COL_BLACK  C(0, 0, 0)
#define COL_WHITE  C(31, 63, 31)
#define COL_GREEN  C(4, 55, 8)
#define COL_GREY   C(12, 24, 12)
#define COL_YELLOW C(31, 60, 4)
#define COL_CYAN   C(6, 50, 28)
#define COL_RED    C(30, 8, 6)

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

static void render(uint32_t tick)
{
    switch (link_state()) {
    case DLINK_UNPAIRED:
        fill(COL_BLACK);
        text_center(8, "espkvm", 2, COL_CYAN);
        text_center(34, "NOT PAIRED", 2, COL_YELLOW);
        text_center(60, "press button to pair", 1, COL_GREY);
        break;

    case DLINK_PAIRING: {
        fill(COL_BLACK);
        text_center(20, "PAIRING", 3, COL_CYAN);
        char dots[4] = "";
        for (uint32_t i = 0; i < (tick / 2) % 4; i++) {
            dots[i] = '.';
        }
        text_center(52, dots, 2, COL_CYAN);
        break;
    }

    case DLINK_RUN:
    default: {
        bool hub_alive = link_ms_since_rx() < 5000;
        bool active = link_is_active() && hub_alive;

        fill(active ? COL_GREEN : COL_BLACK);

        /* The whole point: a huge slot digit readable across the room. */
        char digit[2] = { (char)('0' + (link_slot() % 10)), '\0' };
        text(14, 6, digit, 10, COL_WHITE);   /* 50x70 px */

        text(78, 14, active ? "ACTIVE" : "idle", 2,
             active ? COL_WHITE : COL_GREY);
        text(78, 36, "RCtrl RCtrl", 1, active ? COL_WHITE : COL_GREY);
        text(78, 46, digit, 1, active ? COL_WHITE : COL_GREY);

        if (!hub_alive) {
            fill_rect(72, 60, 88, 14, COL_BLACK);
            text(78, 63, "hub offline", 1, COL_RED);
        } else if (!usb_dev_mounted()) {
            fill_rect(72, 60, 88, 14, COL_BLACK);
            text(78, 63, "USB not up", 1, COL_YELLOW);
        }
        break;
    }
    }
    flush();
}

static void lcd_task(void *arg)
{
    (void)arg;
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
