/*
 * espkvm hub — USB dongle provisioning.
 *
 * The hub's single USB-A port doubles as a flashing station: plug in a
 * T-Dongle-S3 (or any ESP32-S3 dongle board) with its BOOT button held
 * and it enumerates as the Espressif USB-Serial-JTAG ROM bootloader
 * (303A:1001) instead of a HID keyboard. This module watches for that,
 * then streams the dongle firmware stored in the hub's own `dimage`
 * flash partition into the target using esp-serial-flasher. No PC needed
 * to provision new dongles.
 *
 * The stored image is a "pack": kvm_dimage_hdr_t + the merged dongle
 * image (bootloader+partition table+app, flashed at offset 0). CI builds
 * it (tools/gen_dongle_pack.py) and the web-flasher manifest writes it to
 * the hub together with the hub firmware itself.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "esp_crc.h"
#include "esp_log.h"

#include "usb/cdc_acm_host.h"
#include "esp_loader.h"
#include "esp32_usb_cdc_acm_port.h"

#include "usb_kbd.h"
#include "ui.h"
#include "flasher.h"

static const char *TAG = "flasher";

#define POLL_MS        2000
#define CHUNK          4096
#define DIMAGE_SUBTYPE 0x40

static const esp_partition_t *s_part;
static kvm_dimage_hdr_t s_hdr;
static bool s_image_ok;

/* ------------------------------------------------------------------ */

static bool image_validate(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      DIMAGE_SUBTYPE, "dimage");
    if (!s_part) {
        ESP_LOGW(TAG, "no dimage partition — provisioning disabled");
        return false;
    }
    if (esp_partition_read(s_part, 0, &s_hdr, sizeof(s_hdr)) != ESP_OK) {
        return false;
    }
    if (s_hdr.magic != KVM_DIMAGE_MAGIC ||
        s_hdr.len == 0 ||
        s_hdr.len > s_part->size - sizeof(s_hdr)) {
        ESP_LOGW(TAG, "dimage partition empty/invalid — flash the "
                 "dongle pack (see docs/BUILD.md) to enable provisioning");
        return false;
    }

    /* CRC the payload once at startup so a truncated write can never be
     * pushed into a dongle. */
    uint32_t crc = 0;
    static uint8_t buf[CHUNK];
    for (uint32_t off = 0; off < s_hdr.len; off += CHUNK) {
        uint32_t n = (s_hdr.len - off < CHUNK) ? (s_hdr.len - off) : CHUNK;
        if (esp_partition_read(s_part, sizeof(s_hdr) + off, buf, n) != ESP_OK) {
            return false;
        }
        crc = esp_crc32_le(crc, buf, n);
    }
    if (crc != s_hdr.crc32) {
        ESP_LOGE(TAG, "dimage CRC mismatch (stored %08x, computed %08x)",
                 (unsigned)s_hdr.crc32, (unsigned)crc);
        return false;
    }
    ESP_LOGI(TAG, "dongle image ready: %u bytes, fw v%u",
             (unsigned)s_hdr.len, s_hdr.fw_ver);
    return true;
}

/* Returns true when a full flash cycle succeeded. */
static bool flash_target(void)
{
    static uint8_t buf[CHUNK];
    bool ok = false;

    esp_loader_connect_args_t connect = ESP_LOADER_CONNECT_DEFAULT();
    if (esp_loader_connect(&connect) != ESP_LOADER_SUCCESS) {
        ESP_LOGW(TAG, "bootloader present but connect failed");
        return false;
    }

    target_chip_t chip = esp_loader_get_target();
    if (chip != ESP32S3_CHIP) {
        /* The stored image is built for the S3 dongles; refuse anything
         * else rather than brick it. */
        ESP_LOGE(TAG, "target chip %d is not an ESP32-S3 — aborting", chip);
        return false;
    }

    if (esp_loader_flash_start(s_hdr.flash_addr, s_hdr.len,
                               CHUNK) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "flash_start failed");
        return false;
    }

    for (uint32_t off = 0; off < s_hdr.len; off += CHUNK) {
        uint32_t n = (s_hdr.len - off < CHUNK) ? (s_hdr.len - off) : CHUNK;
        if (esp_partition_read(s_part, sizeof(s_hdr) + off, buf, n) != ESP_OK) {
            ESP_LOGE(TAG, "partition read failed at 0x%x", (unsigned)off);
            goto done;
        }
        if (esp_loader_flash_write(buf, n) != ESP_LOADER_SUCCESS) {
            ESP_LOGE(TAG, "flash_write failed at 0x%x", (unsigned)off);
            goto done;
        }
        if ((off / CHUNK) % 16 == 0) {
            ESP_LOGI(TAG, "flashing... %u%%",
                     (unsigned)(off * 100 / s_hdr.len));
        }
    }

    /* true = reboot the target into the fresh firmware */
    ok = esp_loader_flash_finish(true) == ESP_LOADER_SUCCESS;

done:
    ESP_LOGI(TAG, "provisioning %s", ok ? "complete" : "FAILED");
    return ok;
}

static void flasher_task(void *arg)
{
    (void)arg;

    s_image_ok = image_validate();
    if (!s_image_ok) {
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        /* Keyboard connected => the port is busy doing its day job. */
        if (usb_kbd_connected()) {
            continue;
        }

        /* Try to open an Espressif ROM bootloader on the port. The short
         * timeout makes this a cheap poll when nothing is plugged in. */
        const loader_esp32_usb_cdc_acm_config_t cfg = {
            .device_vid = ESPRESSIF_VID,
            .device_pid = ESP_SERIAL_JTAG_PID,
            .connection_timeout_ms = 500,
            .out_buffer_size = 4096,
        };
        if (loader_port_esp32_usb_cdc_acm_init(&cfg) != ESP_LOADER_SUCCESS) {
            continue;   /* nothing (or not a bootloader) on the port */
        }

        ESP_LOGI(TAG, "dongle bootloader detected — provisioning");
        ui_flasher_state(UI_FLASH_BUSY);
        bool ok = flash_target();
        loader_port_esp32_usb_cdc_acm_deinit();
        ui_flasher_state(ok ? UI_FLASH_DONE : UI_FLASH_FAIL);

        /* Give the user time to unplug before we poll again (and avoid
         * re-flashing the same dongle if BOOT is still held). */
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

esp_err_t flasher_init(void)
{
    /* usb_host is already installed by usb_kbd_init(); CDC-ACM is just
     * another class client alongside HID. */
    esp_err_t err = cdc_acm_host_install(NULL);
    if (err != ESP_OK) {
        return err;
    }
    xTaskCreate(flasher_task, "flasher", 6144, NULL, 5, NULL);
    return ESP_OK;
}
