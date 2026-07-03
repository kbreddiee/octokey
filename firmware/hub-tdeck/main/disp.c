/*
 * espkvm hub-tdeck — ST7789 320x240 status screen.
 *
 * Deliberately simple (status only, no on-screen menu — the trackball
 * doesn't drive a cursor precise enough to make menu widgets pleasant,
 * and every action already has a direct physical gesture; see main.c and
 * docs/PAIRING.md). Same plain SPI + DMA-framebuffer approach as the
 * T-Dongle-S3's lcd.c: one big slot digit, ACTIVE/idle, link health, and
 * a one-line legend for the key remaps this board needs (see kbd_i2c.c).
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

#include "kvm_proto.h"
#include "kvm_font.h"
#include "store.h"
#include "link.h"
#include "disp.h"

static const char *TAG = "disp";

#define LCD_W 320
#define LCD_H 240

/* Landscape 320x240 IPS panel, no offset on this batch. If the image is
 * shifted, X_OFF/Y_OFF are the knob; if colors/orientation are wrong,
 * flip the MADCTL byte below (0x60 <-> 0xA0, or drop the BGR bit 0x08). */
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

static spi_device_handle_t s_spi;
static uint16_t *s_fb;
static SemaphoreHandle_t s_lock;
static bool s_ok;

static struct {
    bool pairing;
    int  pair_secs;
    char toast[40];
    int64_t toast_until_us;
} s_state;

/* ------------------------------------------------------------------ */

static void dc(int level) { gpio_set_level(CONFIG_ESPKVM_LCD_DC, level); }

static void cmd8(uint8_t c)
{
    spi_transaction_t t = { .length = 8, .tx_buffer = &c };
    dc(0);
    spi_device_polling_transmit(s_spi, &t);
}

static void data(const uint8_t *d, size_t len)
{
    if (!len) return;
    spi_transaction_t t = { .length = len * 8, .tx_buffer = d };
    dc(1);
    spi_device_polling_transmit(s_spi, &t);
}

static void cmd_d(uint8_t c, const uint8_t *d, size_t len) { cmd8(c); data(d, len); }

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
    uint8_t ca[4] = { X_OFF >> 8, X_OFF & 0xFF, (X_OFF + LCD_W - 1) >> 8, (X_OFF + LCD_W - 1) & 0xFF };
    uint8_t ra[4] = { Y_OFF >> 8, Y_OFF & 0xFF, (Y_OFF + LCD_H - 1) >> 8, (Y_OFF + LCD_H - 1) & 0xFF };
    cmd_d(0x2A, ca, 4);
    cmd_d(0x2B, ra, 4);
    cmd8(0x2C);
    spi_transaction_t t = { .length = LCD_W * LCD_H * 2 * 8, .tx_buffer = s_fb };
    dc(1);
    spi_device_transmit(s_spi, &t);
}

/* ------------------------------------------------------------------ */

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
        text(10, 10, "espkvm", 2, COL_CYAN);

        if (active == KVM_SLOT_NONE) {
            text_center(100, "no dongles paired", 2, COL_GREY);
            text_center(140, "hold trackball 3s to pair", 1, COL_GREY);
        } else {
            const hub_pairing_t *p = store_pairing(active);
            char digit[2] = { (char)('0' + (active % 10)), '\0' };
            bool online = link_slot_online(active);

            text(20, 50, digit, 10, online ? COL_GREEN : COL_RED);
            text(140, 60, p ? p->name : "?", 3, COL_WHITE);
            text(140, 100, online ? "online" : "OFFLINE", 2,
                 online ? COL_GREEN : COL_RED);
        }

        text_center(210, "ALT=ctrl  2xALT+num=switch  SYM+ball=arrows", 1, COL_GREY);
    }

out:
    if (s_state.toast[0] && esp_timer_get_time() < s_state.toast_until_us) {
        fill_rect(0, LCD_H - 26, LCD_W, 26, COL_CYAN);
        text_center(LCD_H - 20, s_state.toast, 1, COL_BLACK);
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
    s_fb = heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_DMA);
    if (!s_fb) return ESP_ERR_NO_MEM;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_ESPKVM_LCD_DC) | (1ULL << CONFIG_ESPKVM_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_ESPKVM_LCD_MOSI,
        .miso_io_num = CONFIG_ESPKVM_LCD_MISO,
        .sclk_io_num = CONFIG_ESPKVM_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * 2 + 16,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = CONFIG_ESPKVM_LCD_CS,
        .queue_size = 2,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev, &s_spi);
    if (err != ESP_OK) return err;

    /* This panel has no dedicated RESET pin broken out on T-Deck (shared
     * with the board's own power-on sequencing) — SLPOUT + a settle
     * delay is sufficient without a hardware reset pulse. */
    cmd8(0x11);                               /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd_d(0x3A, (const uint8_t[]){0x05}, 1);  /* RGB565 */
    cmd_d(0x36, (const uint8_t[]){0x60}, 1);  /* MADCTL: landscape, BGR */
    cmd8(0x21);                               /* INVON */
    cmd8(0x13);                               /* NORON */
    cmd8(0x29);                               /* DISPON */

    fill(COL_BLACK);
    flush();
    gpio_set_level(CONFIG_ESPKVM_LCD_BL, 1);

    s_ok = true;
    xTaskCreate(disp_task, "disp", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "ST7789 up (320x240)");
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
