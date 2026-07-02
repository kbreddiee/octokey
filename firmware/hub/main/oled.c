/* espkvm hub — minimal SSD1306 driver over the IDF v5 i2c_master API. */
/* SPDX-License-Identifier: MIT */

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "kvm_font.h"
#include "oled.h"

static const char *TAG = "oled";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_fb[OLED_W * OLED_H / 8];   /* page-major, 1 bpp */
static bool s_ok;

/* Font comes from the shared kvm_font component. */

/* ------------------------------------------------------------------ */

static esp_err_t cmd(const uint8_t *c, size_t n)
{
    uint8_t buf[32];
    buf[0] = 0x00;   /* Co=0, D/C#=0: command stream */
    memcpy(&buf[1], c, n);
    return i2c_master_transmit(s_dev, buf, n + 1, 100);
}

esp_err_t oled_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_ESPKVM_OLED_SDA,
        .scl_io_num = CONFIG_ESPKVM_OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_ESPKVM_OLED_ADDR,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    static const uint8_t init_seq[] = {
        0xAE,               /* display off                    */
        0xD5, 0x80,         /* clock divide                   */
        0xA8, 0x3F,         /* multiplex = 64                 */
        0xD3, 0x00,         /* display offset                 */
        0x40,               /* start line 0                   */
        0x8D, 0x14,         /* charge pump on                 */
        0x20, 0x00,         /* horizontal addressing          */
        0xA1,               /* segment remap                  */
        0xC8,               /* COM scan direction: remapped   */
        0xDA, 0x12,         /* COM pins: alternative          */
        0x81, 0xCF,         /* contrast                       */
        0xD9, 0xF1,         /* pre-charge                     */
        0xDB, 0x40,         /* VCOMH deselect                 */
        0xA4,               /* resume from RAM                */
        0xA6,               /* normal (not inverted)          */
        0xAF,               /* display on                     */
    };
    err = cmd(init_seq, sizeof(init_seq));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no SSD1306 at 0x%02X (check wiring/pins)",
                 CONFIG_ESPKVM_OLED_ADDR);
        return err;
    }
    s_ok = true;
    oled_clear();
    oled_flush();
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
        uint8_t c = (uint8_t)*s;
        const uint8_t *glyph = kvm_font_glyph((char)c);
        uint16_t base = row * OLED_W + col * 6;
        for (int i = 0; i < 5; i++) {
            s_fb[base + i] = inv ? (uint8_t)~glyph[i] : glyph[i];
        }
        s_fb[base + 5] = inv ? 0xFF : 0x00;   /* inter-char gap */
    }
}

void oled_text_scaled(int x, int y, const char *s, uint8_t scale)
{
    if (scale == 0) {
        scale = 1;
    }
    for (; *s; s++) {
        uint8_t c = (uint8_t)*s;
        const uint8_t *glyph = kvm_font_glyph((char)c);
        for (int gx = 0; gx < 5; gx++) {
            for (int gy = 0; gy < 8; gy++) {
                bool on = (glyph[gx] >> gy) & 1;
                if (!on) {
                    continue;
                }
                oled_fill_rect(x + gx * scale, y + gy * scale,
                               scale, scale, true);
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
    static const uint8_t window[] = {
        0x21, 0x00, 0x7F,   /* columns 0..127 */
        0x22, 0x00, 0x07,   /* pages 0..7     */
    };
    cmd(window, sizeof(window));

    /* One transfer: control byte 0x40 (data stream) + full framebuffer. */
    static uint8_t buf[1 + sizeof(s_fb)];
    buf[0] = 0x40;
    memcpy(&buf[1], s_fb, sizeof(s_fb));
    i2c_master_transmit(s_dev, buf, sizeof(buf), 200);
}
