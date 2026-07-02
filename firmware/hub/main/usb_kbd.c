/*
 * espkvm hub — USB host: HID device handling.
 *
 * Built on the ESP-IDF usb_host stack + Espressif's usb_host_hid class
 * driver. Every HID *interface* (composite receivers expose several:
 * typically boot keyboard, mouse, consumer/system control) is opened
 * independently:
 *
 *   1. On connect we fetch the interface's report descriptor and run it
 *      through kvm_hidparse to build a field map. This is what lets a
 *      Rii-X8-style 2.4 GHz receiver — keyboard + touchpad + media keys
 *      across multiple interfaces and report IDs — work unmodified.
 *   2. Boot-subclass interfaces are switched to *report* protocol first
 *      (per HID 1.11 boot devices default to boot protocol after reset,
 *      which would not match the parsed descriptor). If the descriptor
 *      turns out to be unusable we fall back to fixed boot-format parsing
 *      and switch the interface back to boot protocol.
 *   3. Every interrupt-IN report is decoded against the map into a
 *      normalized kvm_hidp_out_t and handed to the input pipeline.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "usb/usb_host.h"
#include "usb/hid_host.h"

#include "usb_kbd.h"

static const char *TAG = "usb_kbd";

#define MAX_IFACES 6

typedef struct {
    bool used;
    hid_host_device_handle_t handle;
    kvm_hidp_map_t map;
    bool boot_fallback;     /* descriptor unusable — fixed boot parsing   */
    uint8_t boot_proto;     /* HID_PROTOCOL_KEYBOARD / HID_PROTOCOL_MOUSE */
} iface_t;

static iface_t s_ifaces[MAX_IFACES];
static usb_input_cb_t s_cb;
static volatile int s_open_count;

/* Driver events are forwarded out of the HID-host task through this queue
 * so that open/descriptor-fetch (blocking control transfers) never run in
 * the driver's own callback context. */
static QueueHandle_t s_devq;

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
} dev_msg_t;

/* ------------------------------------------------------------------ */

static iface_t *iface_for(hid_host_device_handle_t h)
{
    for (int i = 0; i < MAX_IFACES; i++) {
        if (s_ifaces[i].used && s_ifaces[i].handle == h) {
            return &s_ifaces[i];
        }
    }
    return NULL;
}

/* Fixed-format parsing for boot-protocol devices whose descriptors we
 * could not use. Boot keyboard: [mods, reserved, key x6]. Boot mouse:
 * [buttons, dx, dy, (wheel)]. */
static bool boot_decode(const iface_t *ifc, const uint8_t *data, size_t len,
                        kvm_hidp_out_t *out)
{
    memset(out, 0, sizeof(*out));
    if (ifc->boot_proto == HID_PROTOCOL_KEYBOARD && len >= 8) {
        out->has_kbd = true;
        out->mods = data[0];
        memcpy(out->keys, &data[2], 6);
        return true;
    }
    if (ifc->boot_proto == HID_PROTOCOL_MOUSE && len >= 3) {
        out->has_mouse = true;
        out->buttons = data[0];
        out->dx = (int8_t)data[1];
        out->dy = (int8_t)data[2];
        if (len >= 4) {
            out->wheel = (int8_t)data[3];
        }
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Interface events (input reports)                                   */
/* ------------------------------------------------------------------ */

static void iface_event_cb(hid_host_device_handle_t handle,
                           const hid_host_interface_event_t event,
                           void *arg)
{
    (void)arg;
    uint8_t data[64];
    size_t len = 0;

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        iface_t *ifc = iface_for(handle);
        if (!ifc) {
            break;
        }
        if (hid_host_device_get_raw_input_report_data(handle, data,
                                                      sizeof(data),
                                                      &len) != ESP_OK) {
            break;
        }
        kvm_hidp_out_t out;
        bool ok = ifc->boot_fallback
                ? boot_decode(ifc, data, len, &out)
                : (kvm_hidp_decode(&ifc->map, data, len, &out) == 0);
        if (ok && s_cb) {
            s_cb(&out);
        }
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED: {
        iface_t *ifc = iface_for(handle);
        hid_host_device_close(handle);
        if (ifc) {
            memset(ifc, 0, sizeof(*ifc));
            s_open_count--;
        }
        ESP_LOGI(TAG, "HID interface disconnected (%d open)", s_open_count);
        break;
    }
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Device (driver-level) events                                       */
/* ------------------------------------------------------------------ */

static void open_interface(hid_host_device_handle_t handle)
{
    iface_t *ifc = NULL;
    for (int i = 0; i < MAX_IFACES; i++) {
        if (!s_ifaces[i].used) {
            ifc = &s_ifaces[i];
            break;
        }
    }
    if (!ifc) {
        ESP_LOGW(TAG, "too many HID interfaces, ignoring");
        return;
    }

    const hid_host_device_config_t dev_config = {
        .callback = iface_event_cb,
        .callback_arg = NULL,
    };
    if (hid_host_device_open(handle, &dev_config) != ESP_OK) {
        ESP_LOGW(TAG, "device_open failed");
        return;
    }

    hid_host_dev_params_t params;
    ESP_ERROR_CHECK(hid_host_device_get_params(handle, &params));

    memset(ifc, 0, sizeof(*ifc));
    ifc->used = true;
    ifc->handle = handle;
    ifc->boot_proto = params.proto;

    /* Boot-subclass interfaces power up in boot protocol; put them into
     * report protocol so the report descriptor describes what we receive. */
    if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_REPORT);
    }
    /* Idle 0: only report on change — less traffic, and key-repeat is the
     * host OS's job anyway. Optional per spec, so errors are ignored. */
    hid_class_request_set_idle(handle, 0, 0);

    size_t desc_len = 0;
    uint8_t *desc = hid_host_get_report_descriptor(handle, &desc_len);
    bool parsed = desc && desc_len > 0 &&
                  kvm_hidp_parse(desc, desc_len, &ifc->map) == 0 &&
                  kvm_hidp_map_useful(&ifc->map);

    if (!parsed) {
        if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
            /* Descriptor useless but device speaks boot protocol: use it. */
            ifc->boot_fallback = true;
            hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT);
            ESP_LOGW(TAG, "iface %u: descriptor unusable, boot fallback "
                     "(proto %u)", params.iface_num, params.proto);
        } else {
            ESP_LOGW(TAG, "iface %u: no usable HID fields, ignoring",
                     params.iface_num);
            hid_host_device_close(handle);
            memset(ifc, 0, sizeof(*ifc));
            return;
        }
    }

    if (hid_host_device_start(handle) != ESP_OK) {
        ESP_LOGW(TAG, "device_start failed");
        hid_host_device_close(handle);
        memset(ifc, 0, sizeof(*ifc));
        return;
    }

    s_open_count++;
    ESP_LOGI(TAG, "HID iface %u up: subclass %u proto %u, reports=%u ids=%d "
             "(%d open)", params.iface_num, params.sub_class, params.proto,
             ifc->map.n, ifc->map.use_ids, s_open_count);
}

static void device_event_cb(hid_host_device_handle_t handle,
                            const hid_host_driver_event_t event,
                            void *arg)
{
    (void)arg;
    dev_msg_t m = { .handle = handle, .event = event };
    xQueueSend(s_devq, &m, 0);
}

static void dev_task(void *arg)
{
    (void)arg;
    dev_msg_t m;
    for (;;) {
        if (xQueueReceive(s_devq, &m, portMAX_DELAY) == pdTRUE) {
            if (m.event == HID_HOST_DRIVER_EVENT_CONNECTED) {
                open_interface(m.handle);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* usb_host library task                                              */
/* ------------------------------------------------------------------ */

static void usb_lib_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

esp_err_t usb_kbd_init(usb_input_cb_t cb)
{
    s_cb = cb;
    s_devq = xQueueCreate(8, sizeof(dev_msg_t));
    if (!s_devq) {
        return ESP_ERR_NO_MEM;
    }

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 15, NULL);

    const hid_host_driver_config_t driver_config = {
        .create_background_task = true,
        .task_priority = 14,
        .stack_size = 4096,
        .core_id = 0,
        .callback = device_event_cb,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&driver_config));
    xTaskCreate(dev_task, "hid_open", 4096, NULL, 13, NULL);

    ESP_LOGI(TAG, "USB host up — plug in a keyboard or receiver dongle");
    return ESP_OK;
}

bool usb_kbd_connected(void)
{
    return s_open_count > 0;
}
