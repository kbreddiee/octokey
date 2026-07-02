/* Host unit tests: HID report-descriptor parsing + report decoding,
 * using real-world descriptors. */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <string.h>
#include "kvm_hidparse.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* The canonical boot keyboard descriptor (HID 1.11 appendix B.1). */
static const uint8_t BOOT_KBD[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop)         */
    0x09, 0x06,        /* Usage (Keyboard)                     */
    0xA1, 0x01,        /* Collection (Application)             */
    0x05, 0x07,        /*   Usage Page (Keyboard)              */
    0x19, 0xE0,        /*   Usage Minimum (LCtrl)              */
    0x29, 0xE7,        /*   Usage Maximum (RGui)               */
    0x15, 0x00,        /*   Logical Minimum (0)                */
    0x25, 0x01,        /*   Logical Maximum (1)                */
    0x75, 0x01,        /*   Report Size (1)                    */
    0x95, 0x08,        /*   Report Count (8)                   */
    0x81, 0x02,        /*   Input (Data, Var, Abs) — modifiers */
    0x95, 0x01,        /*   Report Count (1)                   */
    0x75, 0x08,        /*   Report Size (8)                    */
    0x81, 0x01,        /*   Input (Const) — reserved byte      */
    0x95, 0x05,        /*   Report Count (5)                   */
    0x75, 0x01,        /*   Report Size (1)                    */
    0x05, 0x08,        /*   Usage Page (LEDs)                  */
    0x19, 0x01,        /*   Usage Minimum (Num Lock)           */
    0x29, 0x05,        /*   Usage Maximum (Kana)               */
    0x91, 0x02,        /*   Output (LEDs)                      */
    0x95, 0x01,        /*   Report Count (1)                   */
    0x75, 0x03,        /*   Report Size (3)                    */
    0x91, 0x01,        /*   Output (Const) — LED padding       */
    0x95, 0x06,        /*   Report Count (6)                   */
    0x75, 0x08,        /*   Report Size (8)                    */
    0x15, 0x00,        /*   Logical Minimum (0)                */
    0x25, 0x65,        /*   Logical Maximum (101)              */
    0x05, 0x07,        /*   Usage Page (Keyboard)              */
    0x19, 0x00,        /*   Usage Minimum (0)                  */
    0x29, 0x65,        /*   Usage Maximum (101)                */
    0x81, 0x00,        /*   Input (Data, Array) — 6 keys       */
    0xC0,              /* End Collection                       */
};

/* Classic 3-button wheel mouse, no report IDs. */
static const uint8_t WHEEL_MOUSE[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,          /* padding */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0, 0xC0,
};

/* Consumer-control block with a report ID and a 16-bit usage array —
 * the way media keys appear on composite 2.4 GHz receivers. */
static const uint8_t CONSUMER_RID3[] = {
    0x05, 0x0C,        /* Usage Page (Consumer)     */
    0x09, 0x01,        /* Usage (Consumer Control)  */
    0xA1, 0x01,        /* Collection (Application)  */
    0x85, 0x03,        /*   Report ID (3)           */
    0x15, 0x00,        /*   Logical Minimum (0)     */
    0x26, 0xFF, 0x03,  /*   Logical Maximum (1023)  */
    0x19, 0x00,        /*   Usage Minimum (0)       */
    0x2A, 0xFF, 0x03,  /*   Usage Maximum (1023)    */
    0x75, 0x10,        /*   Report Size (16)        */
    0x95, 0x01,        /*   Report Count (1)        */
    0x81, 0x00,        /*   Input (Data, Array)     */
    0xC0,
};

static void test_boot_keyboard(void)
{
    kvm_hidp_map_t map;
    CHECK(kvm_hidp_parse(BOOT_KBD, sizeof(BOOT_KBD), &map) == 0);
    CHECK(kvm_hidp_map_useful(&map));
    CHECK(!map.use_ids);
    CHECK(map.n == 1);
    CHECK(map.r[0].k_mods.present && map.r[0].k_mods.bit == 0);
    CHECK(map.r[0].k_arr.present && map.r[0].k_arr.bit == 16);
    CHECK(map.r[0].k_arr_count == 6);
    CHECK(map.r[0].bits == 64);

    /* LShift + 'a' (0x04) held */
    const uint8_t rpt[8] = {0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
    kvm_hidp_out_t out;
    CHECK(kvm_hidp_decode(&map, rpt, sizeof(rpt), &out) == 0);
    CHECK(out.has_kbd);
    CHECK(out.mods == 0x02);
    CHECK(out.keys[0] == 0x04 && out.keys[1] == 0);
    CHECK(!out.has_mouse && !out.has_consumer);
}

static void test_wheel_mouse(void)
{
    kvm_hidp_map_t map;
    CHECK(kvm_hidp_parse(WHEEL_MOUSE, sizeof(WHEEL_MOUSE), &map) == 0);
    CHECK(kvm_hidp_map_useful(&map));
    CHECK(map.r[0].m_btn.present && map.r[0].m_btn_count == 3);
    CHECK(map.r[0].m_x.present && map.r[0].m_x.bit == 8);
    CHECK(map.r[0].m_y.present && map.r[0].m_y.bit == 16);
    CHECK(map.r[0].m_wheel.present && map.r[0].m_wheel.bit == 24);
    CHECK(map.r[0].m_x.is_signed);

    /* left button, dx=+5, dy=-5, wheel=+1 */
    const uint8_t rpt[4] = {0x01, 0x05, 0xFB, 0x01};
    kvm_hidp_out_t out;
    CHECK(kvm_hidp_decode(&map, rpt, sizeof(rpt), &out) == 0);
    CHECK(out.has_mouse);
    CHECK(out.buttons == 0x01);
    CHECK(out.dx == 5 && out.dy == -5 && out.wheel == 1);
}

static void test_consumer(void)
{
    kvm_hidp_map_t map;
    CHECK(kvm_hidp_parse(CONSUMER_RID3, sizeof(CONSUMER_RID3), &map) == 0);
    CHECK(map.use_ids);
    CHECK(kvm_hidp_map_useful(&map));

    /* report ID 3, usage 0x00E9 (Volume Up) */
    const uint8_t press[3] = {0x03, 0xE9, 0x00};
    kvm_hidp_out_t out;
    CHECK(kvm_hidp_decode(&map, press, sizeof(press), &out) == 0);
    CHECK(out.has_consumer && out.consumer == 0x00E9);

    const uint8_t release[3] = {0x03, 0x00, 0x00};
    CHECK(kvm_hidp_decode(&map, release, sizeof(release), &out) == 0);
    CHECK(out.has_consumer && out.consumer == 0);

    /* Unknown report ID must be dropped */
    const uint8_t alien[3] = {0x09, 0xE9, 0x00};
    CHECK(kvm_hidp_decode(&map, alien, sizeof(alien), &out) == -1);
}

/* A composite descriptor: mouse (RID 1) and consumer (RID 3) on one
 * interface — the shape wireless receiver dongles use. */
static void test_composite(void)
{
    uint8_t desc[sizeof(WHEEL_MOUSE) + 2 + sizeof(CONSUMER_RID3)];
    size_t n = 0;
    /* Splice a Report ID item into the mouse descriptor right after the
     * outer collection opens (bytes 0..5), then the rest. */
    memcpy(desc, WHEEL_MOUSE, 6);
    n += 6;
    desc[n++] = 0x85;   /* Report ID (1) */
    desc[n++] = 0x01;
    memcpy(desc + n, WHEEL_MOUSE + 6, sizeof(WHEEL_MOUSE) - 6);
    n += sizeof(WHEEL_MOUSE) - 6;
    memcpy(desc + n, CONSUMER_RID3, sizeof(CONSUMER_RID3));
    n += sizeof(CONSUMER_RID3);

    kvm_hidp_map_t map;
    CHECK(kvm_hidp_parse(desc, n, &map) == 0);
    CHECK(map.use_ids);
    CHECK(map.n == 2);

    const uint8_t mouse_rpt[5] = {0x01, 0x00, 0x0A, 0x00, 0xFF};
    kvm_hidp_out_t out;
    CHECK(kvm_hidp_decode(&map, mouse_rpt, sizeof(mouse_rpt), &out) == 0);
    CHECK(out.has_mouse && out.dx == 10 && out.wheel == -1);
    CHECK(!out.has_consumer);

    const uint8_t cons_rpt[3] = {0x03, 0xCD, 0x00};   /* Play/Pause */
    CHECK(kvm_hidp_decode(&map, cons_rpt, sizeof(cons_rpt), &out) == 0);
    CHECK(out.has_consumer && out.consumer == 0x00CD);
}

int main(void)
{
    test_boot_keyboard();
    test_wheel_mouse();
    test_consumer();
    test_composite();
    if (failures) {
        printf("test_hidparse: %d FAILURES\n", failures);
        return 1;
    }
    printf("test_hidparse: all ok\n");
    return 0;
}
