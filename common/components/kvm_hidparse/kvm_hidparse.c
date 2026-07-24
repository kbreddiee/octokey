/*
 * espkvm — generic HID report-descriptor parser + report decoder.
 *
 * HID descriptors are a byte-code: a stream of short items that mutate
 * parser state (GLOBAL items persist, LOCAL items apply to the next main
 * item only) until a MAIN "Input" item emits fields into the report. We
 * walk that stream once and record the bit position of every field we care
 * about. See USB HID 1.11 spec §6.2.2.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include "kvm_hidparse.h"

/* Usage pages we understand */
#define PAGE_DESKTOP   0x01
#define PAGE_KEYBOARD  0x07
#define PAGE_LED       0x08
#define PAGE_BUTTON    0x09
#define PAGE_CONSUMER  0x0C

/* Generic Desktop usages */
#define USG_X          0x30
#define USG_Y          0x31
#define USG_WHEEL      0x38
/* Consumer usage that rides inside mouse reports */
#define USG_AC_PAN     0x0238

/* Main-item flag bits (HID 1.11 §6.2.2.5) */
#define FLAG_CONSTANT  0x01   /* padding, no usage                        */
#define FLAG_VARIABLE  0x02   /* one field per usage (bitmap) vs. array   */

#define MAX_LOCAL_USAGES 24
#define GLOBAL_STACK     4

typedef struct {
    uint16_t page;
    int32_t  lmin, lmax;
    uint8_t  rsize;      /* report size (bits per element) */
    uint16_t rcount;     /* report count (elements)        */
} gstate_t;

typedef struct {
    gstate_t g;
    gstate_t gstack[GLOBAL_STACK];
    uint8_t  gsp;

    /* LOCAL state — cleared after every main item */
    uint32_t usages[MAX_LOCAL_USAGES];  /* extended: page<<16 | id */
    uint8_t  n_usages;
    uint32_t umin, umax;
    bool     have_range;

    kvm_hidp_map_t *map;
    kvm_hidp_report_t *cur;   /* report currently being appended to */
} pstate_t;

/* ------------------------------------------------------------------ */

static kvm_hidp_report_t *find_or_add_report(kvm_hidp_map_t *map, uint8_t id)
{
    for (uint8_t i = 0; i < map->n; i++) {
        if (map->r[i].id == id) {
            return &map->r[i];
        }
    }
    if (map->n >= KVM_HIDP_MAX_REPORTS) {
        return NULL;
    }
    kvm_hidp_report_t *r = &map->r[map->n++];
    memset(r, 0, sizeof(*r));
    r->id = id;
    return r;
}

static void clear_locals(pstate_t *p)
{
    p->n_usages   = 0;
    p->umin = p->umax = 0;
    p->have_range = false;
}

/* Extended usage for element `el` of a variable main item. Per the spec,
 * a usage range enumerates element-wise; an explicit usage list applies
 * element-wise with the last usage repeating if the list is short. */
static uint32_t element_usage(const pstate_t *p, uint16_t el)
{
    if (p->have_range) {
        return p->umin + el;
    }
    if (p->n_usages == 0) {
        return 0;
    }
    uint8_t i = (el < p->n_usages) ? (uint8_t)el : (uint8_t)(p->n_usages - 1);
    return p->usages[i];
}

static void set_field(kvm_hidp_field_t *f, uint16_t bit, uint8_t size,
                      bool is_signed)
{
    if (f->present) {
        return;   /* first definition wins */
    }
    f->bit = bit;
    f->size = size;
    f->is_signed = is_signed;
    f->present = true;
}

/* One MAIN Input item: allocate rcount fields of rsize bits each starting
 * at the report's current bit cursor, and classify them. */
static void process_input(pstate_t *p, uint32_t flags)
{
    kvm_hidp_report_t *rep = p->cur;
    if (!rep) {
        return;
    }
    const gstate_t *g = &p->g;
    uint16_t base = rep->bits;
    uint32_t total = (uint32_t)g->rsize * g->rcount;
    bool sgn = g->lmin < 0;

    if (total == 0 || rep->bits + total > 2048) {
        goto done;   /* degenerate or absurd descriptor — keep cursor sane */
    }

    if (flags & FLAG_CONSTANT) {
        rep->bits += total;   /* padding: consumes bits, defines nothing */
        goto done;
    }

    if (flags & FLAG_VARIABLE) {
        /* Bitmap/per-usage fields: element i lives at base + i*rsize. */
        uint32_t u0 = element_usage(p, 0);
        uint16_t page0 = (uint16_t)(u0 >> 16);
        uint16_t id0 = (uint16_t)u0;

        if (page0 == PAGE_KEYBOARD) {
            if (p->have_range && p->umin == ((PAGE_KEYBOARD << 16) | 0xE0) &&
                g->rcount == 8 && g->rsize == 1) {
                /* The classic modifier byte (LCtrl..RGui). */
                set_field(&rep->k_mods, base, 1, false);
            } else if (p->have_range && g->rsize == 1) {
                /* NKRO bitmap: bit i => usage umin+i is pressed. */
                if (!rep->k_bmp.present) {
                    set_field(&rep->k_bmp, base, 1, false);
                    rep->k_bmp_umin = (uint16_t)p->umin;
                    rep->k_bmp_count = g->rcount;
                }
            } else if (id0 == 0xE0 && g->rcount == 8 && g->rsize == 1) {
                /* Modifiers given as an explicit usage list. */
                set_field(&rep->k_mods, base, 1, false);
            }
        } else if (page0 == PAGE_BUTTON) {
            if (!rep->m_btn.present && g->rsize == 1) {
                set_field(&rep->m_btn, base, 1, false);
                rep->m_btn_count = (g->rcount > 8) ? 8 : (uint8_t)g->rcount;
            }
        } else {
            /* Mixed pages possible per element (e.g. X, Y then AC Pan). */
            for (uint16_t el = 0; el < g->rcount; el++) {
                uint32_t u = element_usage(p, el);
                uint16_t pg = (uint16_t)(u >> 16);
                uint16_t id = (uint16_t)u;
                uint16_t bit = base + el * g->rsize;

                if (pg == PAGE_DESKTOP) {
                    if (id == USG_X)     set_field(&rep->m_x, bit, g->rsize, sgn);
                    if (id == USG_Y)     set_field(&rep->m_y, bit, g->rsize, sgn);
                    if (id == USG_WHEEL) set_field(&rep->m_wheel, bit, g->rsize, sgn);
                } else if (pg == PAGE_CONSUMER) {
                    if (id == USG_AC_PAN) {
                        set_field(&rep->m_pan, bit, g->rsize, sgn);
                    } else if (g->rsize == 1 &&
                               rep->c_bit_n < KVM_HIDP_MAX_CBITS) {
                        /* Media keys as individual bits. */
                        rep->c_bit[rep->c_bit_n].usage = id;
                        rep->c_bit[rep->c_bit_n].bit = bit;
                        rep->c_bit_n++;
                    }
                }
            }
        }
        rep->bits += total;
        goto done;
    }

    /* ARRAY item: each element's *value* selects a usage
     * (usage = umin + value - lmin; typically both are 0 => identity).
     * This is how 6KRO keyboards and most consumer blocks work. */
    {
        uint32_t u0 = p->have_range ? p->umin
                    : (p->n_usages ? p->usages[0] : 0);
        uint16_t page0 = (uint16_t)(u0 >> 16);
        uint16_t umin_id = p->have_range ? (uint16_t)p->umin : 0;

        if (page0 == PAGE_KEYBOARD && !rep->k_arr.present) {
            set_field(&rep->k_arr, base, g->rsize, false);
            rep->k_arr_count = (g->rcount > 16) ? 16 : (uint8_t)g->rcount;
            rep->k_arr_lmin = g->lmin;
            rep->k_arr_umin = umin_id;
        } else if (page0 == PAGE_CONSUMER && !rep->c_arr.present) {
            set_field(&rep->c_arr, base, g->rsize, false);
            rep->c_arr_count = (g->rcount > 8) ? 8 : (uint8_t)g->rcount;
            rep->c_arr_lmin = g->lmin;
            rep->c_arr_umin = umin_id;
        }
        rep->bits += total;
    }

done:
    clear_locals(p);
}

int kvm_hidp_parse(const uint8_t *desc, size_t len, kvm_hidp_map_t *map)
{
    if (!desc || !map) {
        return -1;
    }
    memset(map, 0, sizeof(*map));

    pstate_t p;
    memset(&p, 0, sizeof(p));
    p.map = map;
    p.g.lmax = 1;

    size_t i = 0;
    while (i < len) {
        uint8_t prefix = desc[i++];

        if (prefix == 0xFE) {
            /* Long item: [0xFE][size][tag][data...] — never used by real
             * input devices; skip defensively. */
            if (i + 2 > len) return -1;
            uint8_t lsize = desc[i];
            i += 2 + lsize;
            continue;
        }

        uint8_t dsize = prefix & 0x03;
        if (dsize == 3) dsize = 4;
        uint8_t itype = (prefix >> 2) & 0x03;   /* 0 main, 1 global, 2 local */
        uint8_t tag   = prefix >> 4;

        if (i + dsize > len) {
            return -1;   /* truncated item */
        }

        /* Item data, little-endian; also sign-extended for logical min/max. */
        uint32_t uval = 0;
        for (uint8_t b = 0; b < dsize; b++) {
            uval |= (uint32_t)desc[i + b] << (8 * b);
        }
        int32_t sval = (int32_t)uval;
        if (dsize == 1 && (uval & 0x80))       sval = (int32_t)(uval | 0xFFFFFF00u);
        else if (dsize == 2 && (uval & 0x8000)) sval = (int32_t)(uval | 0xFFFF0000u);
        i += dsize;

        switch (itype) {
        case 1:   /* GLOBAL */
            switch (tag) {
            case 0: p.g.page = (uint16_t)uval; break;
            case 1: p.g.lmin = sval; break;
            case 2: p.g.lmax = sval; break;
            case 7: p.g.rsize = (uint8_t)uval; break;
            case 9: p.g.rcount = (uint16_t)uval; break;
            case 8:   /* REPORT ID: switch the report being built */
                map->use_ids = true;
                p.cur = find_or_add_report(map, (uint8_t)uval);
                break;
            case 10:  /* PUSH */
                if (p.gsp < GLOBAL_STACK) p.gstack[p.gsp++] = p.g;
                break;
            case 11:  /* POP */
                if (p.gsp > 0) p.g = p.gstack[--p.gsp];
                break;
            default: break;
            }
            break;

        case 2:   /* LOCAL */
            switch (tag) {
            case 0:   /* USAGE — 4-byte form carries its own page */
                if (p.n_usages < MAX_LOCAL_USAGES) {
                    p.usages[p.n_usages++] = (dsize == 4)
                        ? uval : (((uint32_t)p.g.page << 16) | uval);
                }
                break;
            case 1:   /* USAGE MINIMUM */
                p.umin = (dsize == 4) ? uval
                       : (((uint32_t)p.g.page << 16) | uval);
                p.have_range = true;
                break;
            case 2:   /* USAGE MAXIMUM */
                p.umax = (dsize == 4) ? uval
                       : (((uint32_t)p.g.page << 16) | uval);
                p.have_range = true;
                break;
            default: break;
            }
            break;

        case 0:   /* MAIN */
            switch (tag) {
            case 8:   /* INPUT */
                if (!p.cur && !map->use_ids) {
                    /* Interfaces without report IDs get a single implicit
                     * report with id 0, created lazily.
                     *
                     * This has to hang off INPUT rather than any MAIN item:
                     * descriptors open their Collection *before* declaring
                     * Report ID, so keying it on collections would mint a
                     * phantom id-0 report for every descriptor that uses
                     * IDs at all. */
                    p.cur = find_or_add_report(map, 0);
                }
                process_input(&p, uval);
                break;
            case 9:   /* OUTPUT (e.g. keyboard LEDs) — separate bit space */
            case 11:  /* FEATURE */
            case 10:  /* COLLECTION open */
            case 12:  /* END COLLECTION */
                clear_locals(&p);
                break;
            default:
                clear_locals(&p);
                break;
            }
            break;

        default:
            break;
        }
    }

    return 0;
}

bool kvm_hidp_map_useful(const kvm_hidp_map_t *map)
{
    for (uint8_t i = 0; i < map->n; i++) {
        const kvm_hidp_report_t *r = &map->r[i];
        if (r->k_mods.present || r->k_arr.present || r->k_bmp.present ||
            r->m_x.present || r->m_btn.present ||
            r->c_arr.present || r->c_bit_n > 0) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Report decoding                                                    */
/* ------------------------------------------------------------------ */

/* HID fields are bit-packed little-endian: bit N of the report is bit
 * (N % 8) of byte (N / 8), and multi-bit fields start at their lowest bit. */
static uint32_t get_bits(const uint8_t *b, size_t blen,
                         uint32_t bit, uint8_t size)
{
    uint32_t v = 0;
    for (uint8_t i = 0; i < size && i < 32; i++) {
        uint32_t n = bit + i;
        if (n / 8 >= blen) {
            break;   /* out of range bits read as 0 — decode stays safe */
        }
        v |= (uint32_t)((b[n / 8] >> (n % 8)) & 1u) << i;
    }
    return v;
}

static int32_t get_field(const uint8_t *b, size_t blen,
                         const kvm_hidp_field_t *f)
{
    uint32_t v = get_bits(b, blen, f->bit, f->size);
    if (f->is_signed && f->size < 32 && (v & (1u << (f->size - 1)))) {
        v |= 0xFFFFFFFFu << f->size;   /* sign-extend */
    }
    return (int32_t)v;
}

static int16_t clamp16(int32_t v)
{
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static int8_t clamp8(int32_t v)
{
    if (v > 127)  return 127;
    if (v < -128) return -128;
    return (int8_t)v;
}

int kvm_hidp_decode(const kvm_hidp_map_t *map,
                    const uint8_t *data, size_t len,
                    kvm_hidp_out_t *out)
{
    if (!map || !data || !out || len == 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    uint8_t rid = 0;
    if (map->use_ids) {
        rid = data[0];
        data++;
        len--;
        if (len == 0) {
            return -1;
        }
    }

    const kvm_hidp_report_t *r = NULL;
    for (uint8_t i = 0; i < map->n; i++) {
        if (map->r[i].id == rid) {
            r = &map->r[i];
            break;
        }
    }
    if (!r) {
        return -1;   /* report ID we did not map — drop */
    }

    /* --- keyboard ------------------------------------------------- */
    if (r->k_mods.present || r->k_arr.present || r->k_bmp.present) {
        out->has_kbd = true;
        uint8_t nkeys = 0;

        if (r->k_mods.present) {
            out->mods = (uint8_t)get_bits(data, len, r->k_mods.bit, 8);
        }
        if (r->k_arr.present) {
            for (uint8_t k = 0; k < r->k_arr_count; k++) {
                uint32_t raw = get_bits(data, len,
                                        r->k_arr.bit + k * r->k_arr.size,
                                        r->k_arr.size);
                if (raw == 0) {
                    continue;
                }
                /* array value -> usage id */
                int32_t usage = (int32_t)r->k_arr_umin +
                                ((int32_t)raw - r->k_arr_lmin);
                if (usage > 0 && usage <= 0xFF &&
                    nkeys < KVM_HIDP_MAX_KEYS) {
                    out->keys[nkeys++] = (uint8_t)usage;
                }
            }
        }
        if (r->k_bmp.present) {
            for (uint16_t k = 0; k < r->k_bmp_count; k++) {
                if (!get_bits(data, len, r->k_bmp.bit + k, 1)) {
                    continue;
                }
                uint16_t usage = r->k_bmp_umin + k;
                if (usage >= 0xE0 && usage <= 0xE7) {
                    out->mods |= (uint8_t)(1u << (usage - 0xE0));
                } else if (usage <= 0xFF && nkeys < KVM_HIDP_MAX_KEYS) {
                    out->keys[nkeys++] = (uint8_t)usage;
                }
            }
        }
    }

    /* --- mouse ----------------------------------------------------- */
    if (r->m_btn.present || r->m_x.present || r->m_y.present ||
        r->m_wheel.present || r->m_pan.present) {
        out->has_mouse = true;
        if (r->m_btn.present) {
            out->buttons = (uint8_t)get_bits(data, len, r->m_btn.bit,
                                             r->m_btn_count);
        }
        if (r->m_x.present)     out->dx = clamp16(get_field(data, len, &r->m_x));
        if (r->m_y.present)     out->dy = clamp16(get_field(data, len, &r->m_y));
        if (r->m_wheel.present) out->wheel = clamp8(get_field(data, len, &r->m_wheel));
        if (r->m_pan.present)   out->pan = clamp8(get_field(data, len, &r->m_pan));
    }

    /* --- consumer --------------------------------------------------- */
    if (r->c_arr.present || r->c_bit_n > 0) {
        out->has_consumer = true;
        out->consumer = 0;
        if (r->c_arr.present) {
            for (uint8_t k = 0; k < r->c_arr_count; k++) {
                uint32_t raw = get_bits(data, len,
                                        r->c_arr.bit + k * r->c_arr.size,
                                        r->c_arr.size);
                if (raw != 0) {
                    int32_t usage = (int32_t)r->c_arr_umin +
                                    ((int32_t)raw - r->c_arr_lmin);
                    if (usage > 0 && usage <= 0xFFFF) {
                        out->consumer = (uint16_t)usage;
                        break;   /* forward the first held usage */
                    }
                }
            }
        }
        if (out->consumer == 0) {
            for (uint8_t k = 0; k < r->c_bit_n; k++) {
                if (get_bits(data, len, r->c_bit[k].bit, 1)) {
                    out->consumer = r->c_bit[k].usage;
                    break;
                }
            }
        }
    }

    return 0;
}
