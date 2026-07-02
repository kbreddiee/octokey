/*
 * espkvm dongle — TinyUSB composite HID device.
 *
 * Two HID interfaces, chosen deliberately:
 *   Interface 0: boot-protocol keyboard, no report IDs. BIOSes, UEFI setup
 *                screens and locked-down POS terminals that only speak boot
 *                protocol get a fully working keyboard.
 *   Interface 1: mouse (report ID 1, 16-bit relative X/Y so fast flicks
 *                from high-DPI touchpads don't clip) + consumer control
 *                (report ID 2, media keys).
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "tinyusb.h"
#include "class/hid/hid_device.h"

#include "usb_dev.h"

static const char *TAG = "usb_dev";

enum { ITF_KBD = 0, ITF_MISC = 1, ITF_TOTAL };
enum { RID_MOUSE = 1, RID_CONSUMER = 2 };

/* ------------------------------------------------------------------ */
/* Descriptors                                                        */
/* ------------------------------------------------------------------ */

/* Standard boot-compatible keyboard (8-byte reports, 6KRO). */
static const uint8_t desc_hid_kbd[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

/* Mouse with 16-bit relative X/Y + wheel + AC Pan, then consumer control. */
static const uint8_t desc_hid_misc[] = {
    HID_USAGE_PAGE(HID_USAGE_PAGE_DESKTOP),
    HID_USAGE(HID_USAGE_DESKTOP_MOUSE),
    HID_COLLECTION(HID_COLLECTION_APPLICATION),
        HID_REPORT_ID(RID_MOUSE)
        HID_USAGE(HID_USAGE_DESKTOP_POINTER),
        HID_COLLECTION(HID_COLLECTION_PHYSICAL),
            /* 8 buttons */
            HID_USAGE_PAGE(HID_USAGE_PAGE_BUTTON),
            HID_USAGE_MIN(1), HID_USAGE_MAX(8),
            HID_LOGICAL_MIN(0), HID_LOGICAL_MAX(1),
            HID_REPORT_COUNT(8), HID_REPORT_SIZE(1),
            HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
            /* X, Y: 16-bit relative */
            HID_USAGE_PAGE(HID_USAGE_PAGE_DESKTOP),
            HID_USAGE(HID_USAGE_DESKTOP_X),
            HID_USAGE(HID_USAGE_DESKTOP_Y),
            HID_LOGICAL_MIN_N(-32767, 2), HID_LOGICAL_MAX_N(32767, 2),
            HID_REPORT_COUNT(2), HID_REPORT_SIZE(16),
            HID_INPUT(HID_DATA | HID_VARIABLE | HID_RELATIVE),
            /* vertical wheel: 8-bit relative */
            HID_USAGE(HID_USAGE_DESKTOP_WHEEL),
            HID_LOGICAL_MIN(0x81), HID_LOGICAL_MAX(0x7f),
            HID_REPORT_COUNT(1), HID_REPORT_SIZE(8),
            HID_INPUT(HID_DATA | HID_VARIABLE | HID_RELATIVE),
            /* horizontal wheel (AC Pan): 8-bit relative */
            HID_USAGE_PAGE_N(HID_USAGE_PAGE_CONSUMER, 2),
            HID_USAGE_N(HID_USAGE_CONSUMER_AC_PAN, 2),
            HID_LOGICAL_MIN(0x81), HID_LOGICAL_MAX(0x7f),
            HID_REPORT_COUNT(1), HID_REPORT_SIZE(8),
            HID_INPUT(HID_DATA | HID_VARIABLE | HID_RELATIVE),
        HID_COLLECTION_END,
    HID_COLLECTION_END,

    /* Consumer control: one 16-bit usage value (media keys). */
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(RID_CONSUMER))
};

static const tusb_desc_device_t desc_device = {
    .bLength         = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB          = 0x0200,
    .bDeviceClass    = 0x00,   /* per-interface */
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    /* Espressif VID with a project-local PID. If you redistribute hardware
     * commercially, request a PID at https://github.com/espressif/usb-pids */
    .idVendor        = 0x303A,
    .idProduct       = 0x4B4D,   /* "KM" */
    .bcdDevice       = 0x0100,
    .iManufacturer   = 0x01,
    .iProduct        = 0x02,
    .iSerialNumber   = 0x03,
    .bNumConfigurations = 0x01,
};

#define EPNUM_KBD   0x81
#define EPNUM_MISC  0x82
#define CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN)

static const uint8_t desc_configuration[] = {
    /* config, interface count, string idx, total len, attributes, power (mA) */
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CFG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    /* itf, string idx, boot protocol, report desc len, EP-IN, size, poll ms */
    TUD_HID_DESCRIPTOR(ITF_KBD, 4, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(desc_hid_kbd), EPNUM_KBD, 16, 1),
    TUD_HID_DESCRIPTOR(ITF_MISC, 5, HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_misc), EPNUM_MISC, 16, 1),
};

static char s_serial[13];   /* efuse MAC as hex */

static const char *desc_strings[] = {
    (const char[]){0x09, 0x04},   /* 0: language = English (US) */
    "espkvm",                     /* 1: manufacturer */
    "espkvm KVM dongle",          /* 2: product */
    s_serial,                     /* 3: serial */
    "espkvm Keyboard",            /* 4: HID itf 0 */
    "espkvm Mouse/Media",         /* 5: HID itf 1 */
};

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

static SemaphoreHandle_t s_lock;
static volatile bool s_kbd_held;
static volatile bool s_btn_held;
static volatile bool s_consumer_held;

/* ------------------------------------------------------------------ */
/* TinyUSB callbacks (required by the HID class driver)               */
/* ------------------------------------------------------------------ */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return (instance == ITF_KBD) ? desc_hid_kbd : desc_hid_misc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer; (void)reqlen;
    return 0;   /* not supported — host will use interrupt IN data */
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    /* Keyboard LED output report (Caps/Num lock). We accept and ignore it;
     * echoing LED state back to the physical keyboard through the hub is a
     * possible future feature. */
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer; (void)bufsize;
}

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

esp_err_t usb_dev_init(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    s_lock = xSemaphoreCreateMutex();

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor        = &desc_device,
        .string_descriptor        = desc_strings,
        .string_descriptor_count  = sizeof(desc_strings) / sizeof(desc_strings[0]),
        .external_phy             = false,
        .configuration_descriptor = desc_configuration,
    };
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "USB HID composite up (serial %s)", s_serial);
    }
    return err;
}

bool usb_dev_mounted(void)
{
    return tud_mounted();
}

bool usb_dev_anything_held(void)
{
    return s_kbd_held || s_btn_held || s_consumer_held;
}

/* If the host suspended us (screen locked, S3 sleep...), a HID event must
 * generate a remote-wakeup instead of a report. */
static bool wake_if_suspended(void)
{
    if (tud_suspended()) {
        tud_remote_wakeup();
        return true;
    }
    return false;
}

void usb_dev_kbd(uint8_t mods, const uint8_t keys[6])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool held = mods != 0;
    for (int i = 0; i < 6; i++) {
        held = held || keys[i] != 0;
    }
    s_kbd_held = held;

    if (!wake_if_suspended() && tud_hid_n_ready(ITF_KBD)) {
        tud_hid_n_keyboard_report(ITF_KBD, 0, mods, (uint8_t *)keys);
    }
    xSemaphoreGive(s_lock);
}

void usb_dev_mouse(uint8_t buttons, int16_t dx, int16_t dy,
                   int8_t wheel, int8_t pan)
{
    struct __attribute__((packed)) {
        uint8_t buttons;
        int16_t x, y;
        int8_t  wheel, pan;
    } rpt = { buttons, dx, dy, wheel, pan };

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_btn_held = buttons != 0;
    if (!wake_if_suspended() && tud_hid_n_ready(ITF_MISC)) {
        tud_hid_n_report(ITF_MISC, RID_MOUSE, &rpt, sizeof(rpt));
    }
    xSemaphoreGive(s_lock);
}

void usb_dev_consumer(uint16_t usage)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_consumer_held = usage != 0;
    if (!wake_if_suspended() && tud_hid_n_ready(ITF_MISC)) {
        tud_hid_n_report(ITF_MISC, RID_CONSUMER, &usage, sizeof(usage));
    }
    xSemaphoreGive(s_lock);
}

void usb_dev_release_all(void)
{
    /* Sent on slot switch and by the link-loss watchdog so no key or
     * button can ever stay stuck on the target machine. */
    static const uint8_t no_keys[6] = {0};
    usb_dev_kbd(0, no_keys);
    usb_dev_mouse(0, 0, 0, 0, 0);
    usb_dev_consumer(0);
}
