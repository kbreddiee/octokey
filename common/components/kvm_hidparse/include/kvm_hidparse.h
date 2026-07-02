/*
 * espkvm — generic HID report-descriptor parser.
 *
 * The hub cannot assume boot protocol: wireless-keyboard receiver dongles
 * (e.g. the Rii X8's) are composite devices that put a keyboard, a mouse
 * and consumer-control (media keys) on multiple interfaces and report IDs,
 * with arbitrary field widths and padding. So we parse each interface's
 * report descriptor once at enumeration time into a compact "field map"
 * (bit offset + width of every field we care about), then decode incoming
 * interrupt reports against that map in O(1).
 *
 * Supported field shapes (covers every keyboard/mouse/receiver we know of):
 *   keyboard  - 8-bit modifier block (usages E0..E7, variable bits)
 *             - 6KRO-style key array (array items whose values are usages)
 *             - NKRO bitmaps (variable bits over a usage range)
 *   mouse     - button bitmap (Button page), X/Y/wheel of any width
 *               (8/12/16-bit, signed), AC Pan (horizontal wheel)
 *   consumer  - usage arrays (value = usage id) and single-bit bitmaps
 *
 * Pure C, no ESP-IDF includes — unit-tested on the host (tests/host/).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_HIDP_MAX_REPORTS  8   /* distinct report IDs per interface   */
#define KVM_HIDP_MAX_CBITS    16  /* tracked consumer bitmap bits        */
#define KVM_HIDP_MAX_KEYS     6   /* keys reported downstream (boot-ish) */

/* One extracted field: position inside the (id-stripped) report, in bits. */
typedef struct {
    uint16_t bit;        /* offset from start of report payload           */
    uint8_t  size;       /* width in bits of one element                  */
    bool     present;
    bool     is_signed;  /* logical minimum was negative                  */
} kvm_hidp_field_t;

typedef struct {
    uint8_t  id;         /* report ID, 0 when the interface uses none     */
    uint16_t bits;       /* total input bits (excludes the ID byte)       */

    /* keyboard */
    kvm_hidp_field_t k_mods;                    /* 8x1 bits, E0..E7       */
    kvm_hidp_field_t k_arr;                     /* key usage array        */
    uint8_t          k_arr_count;
    int32_t          k_arr_lmin;                /* logical min of array   */
    uint16_t         k_arr_umin;                /* usage min of array     */
    kvm_hidp_field_t k_bmp;                     /* NKRO bitmap            */
    uint16_t         k_bmp_umin;
    uint16_t         k_bmp_count;

    /* mouse */
    kvm_hidp_field_t m_btn;
    uint8_t          m_btn_count;
    kvm_hidp_field_t m_x, m_y, m_wheel, m_pan;

    /* consumer */
    kvm_hidp_field_t c_arr;
    uint8_t          c_arr_count;
    int32_t          c_arr_lmin;
    uint16_t         c_arr_umin;
    struct { uint16_t usage; uint16_t bit; } c_bit[KVM_HIDP_MAX_CBITS];
    uint8_t          c_bit_n;
} kvm_hidp_report_t;

typedef struct {
    kvm_hidp_report_t r[KVM_HIDP_MAX_REPORTS];
    uint8_t           n;
    bool              use_ids;
} kvm_hidp_map_t;

/* Normalised output of decoding one input report. A single report may
 * carry several sections (composite report IDs are separate reports, but
 * e.g. mouse + pan live in one). */
typedef struct {
    bool     has_kbd;
    uint8_t  mods;
    uint8_t  keys[KVM_HIDP_MAX_KEYS];

    bool     has_mouse;
    uint8_t  buttons;
    int16_t  dx, dy;
    int8_t   wheel, pan;

    bool     has_consumer;
    uint16_t consumer;   /* usage currently held, 0 = none */
} kvm_hidp_out_t;

/* Parse a report descriptor. Returns 0 on success, -1 on malformed input. */
int kvm_hidp_parse(const uint8_t *desc, size_t len, kvm_hidp_map_t *map);

/* True if the map contains at least one keyboard/mouse/consumer section. */
bool kvm_hidp_map_useful(const kvm_hidp_map_t *map);

/* Decode a raw interrupt-IN report (report ID byte included when the
 * interface uses IDs). Returns 0 and fills `out`, or -1 to drop. */
int kvm_hidp_decode(const kvm_hidp_map_t *map,
                    const uint8_t *data, size_t len,
                    kvm_hidp_out_t *out);

#ifdef __cplusplus
}
#endif
