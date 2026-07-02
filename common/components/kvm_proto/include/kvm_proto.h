/*
 * espkvm — over-the-air packet format (hub <-> dongle), protocol v1.
 *
 * Everything in this header is pure, portable C so it can be unit-tested on
 * the host (see tests/host/). No ESP-IDF includes allowed here.
 *
 * Transport: ESP-NOW. All KEYBOARD/MOUSE/CONSUMER/CONTROL traffic is sent to
 * a unicast peer registered with encrypt=true, i.e. it is CCMP-encrypted with
 * the per-peer LMK negotiated at pairing time. Only PAIR_* packets travel in
 * plaintext (they carry only public keys / AEAD ciphertext, never secrets).
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

#define KVM_MAGIC          0xA7
#define KVM_PROTO_VERSION  0x01

#define KVM_MAX_SLOTS      10
#define KVM_SLOT_NONE      0xFF
#define KVM_NAME_LEN       12   /* slot display name, NUL-padded */

/* Crypto material sizes (see kvm_crypto + docs/SECURITY.md) */
#define KVM_PUBKEY_LEN     32   /* X25519 public key                       */
#define KVM_KEY_LEN        16   /* PMK / LMK / KEK — AES-128 keys          */
#define KVM_NONCE_LEN      12   /* AES-GCM nonce                           */
#define KVM_TAG_LEN        16   /* AES-GCM tag / pairing proof             */
/* PAIR_CONFIRM encrypted blob plaintext: PMK[16] | slot | wifi_channel */
#define KVM_PAIR_BLOB_LEN  (KVM_KEY_LEN + 2)

/* ---------------------------------------------------------------------- */
/* Packet types                                                           */
/* ---------------------------------------------------------------------- */

typedef enum {
    /* Input traffic, hub -> dongle, always LMK-encrypted */
    KVM_PKT_KEYBOARD     = 0x01,
    KVM_PKT_MOUSE        = 0x02,
    KVM_PKT_CONSUMER     = 0x03,
    KVM_PKT_CONTROL      = 0x04,
    /* Pairing handshake, plaintext broadcast/unicast on the fixed channel */
    KVM_PKT_PAIR_BEACON  = 0x10,  /* hub -> broadcast                     */
    KVM_PKT_PAIR_REQ     = 0x11,  /* dongle -> hub                        */
    KVM_PKT_PAIR_CONFIRM = 0x12,  /* hub -> dongle                        */
    KVM_PKT_PAIR_DONE    = 0x13,  /* dongle -> hub (LMK-encrypted)        */
} kvm_pkt_type_t;

typedef enum {
    KVM_CTL_RELEASE_ALL = 0x01,   /* dongle must zero kbd/mouse/consumer  */
    KVM_CTL_PING        = 0x02,   /* keepalive; MAC-layer ACK = liveness  */
    KVM_CTL_UNPAIR      = 0x03,   /* dongle must forget pairing + reboot  */
} kvm_ctl_op_t;

/* ---------------------------------------------------------------------- */
/* Wire structs — packed, little-endian (both chips are LE)               */
/* ---------------------------------------------------------------------- */

#pragma pack(push, 1)

/*
 * epoch/seq implement replay protection on top of ESP-NOW's CCMP:
 *  - `epoch` is a boot counter persisted in the hub's NVS (increments each
 *    hub boot). The dongle persists the highest epoch it has seen and
 *    rejects anything older, so captured ciphertext from a previous hub
 *    boot can never be replayed.
 *  - `seq` is monotonic per epoch; the dongle keeps a 64-packet sliding
 *    window (kvm_replay_t below) to reject duplicates within an epoch.
 * Pairing packets set epoch = seq = 0; their freshness comes from the
 * random `session` id and ephemeral ECDH keys instead.
 */
/* Header flag bits */
#define KVM_FLAG_ACTIVE 0x01  /* hub -> dongle: "you are the active slot"
                               * (drives the dongle's LCD/LED indicator)  */

typedef struct {
    uint8_t  magic;      /* KVM_MAGIC                                     */
    uint8_t  version;    /* KVM_PROTO_VERSION                             */
    uint8_t  type;       /* kvm_pkt_type_t                                */
    uint8_t  flags;      /* KVM_FLAG_* (receivers ignore unknown bits)    */
    uint32_t epoch;
    uint32_t seq;
} kvm_hdr_t;

/* Boot-protocol-shaped keyboard state (max 6 concurrent keys + modifiers) */
typedef struct {
    kvm_hdr_t h;
    uint8_t   mods;      /* bit0 LCtrl .. bit7 RGui (HID usage E0..E7)    */
    uint8_t   reserved;
    uint8_t   keys[6];   /* HID usage codes, 0 = empty                    */
} kvm_pkt_kbd_t;

typedef struct {
    kvm_hdr_t h;
    uint8_t   buttons;   /* bit0 = left, bit1 = right, bit2 = middle ...  */
    int16_t   dx, dy;    /* relative; hub normalises any source width     */
    int8_t    wheel;     /* vertical scroll                               */
    int8_t    pan;       /* horizontal scroll (AC Pan)                    */
} kvm_pkt_mouse_t;

typedef struct {
    kvm_hdr_t h;
    uint16_t  usage;     /* Consumer-page usage while held, 0 = release   */
} kvm_pkt_consumer_t;

typedef struct {
    kvm_hdr_t h;
    uint8_t   op;        /* kvm_ctl_op_t                                  */
} kvm_pkt_control_t;

typedef struct {
    kvm_hdr_t h;
    uint32_t  session;               /* random per pairing-mode entry     */
    uint8_t   hub_pub[KVM_PUBKEY_LEN];  /* ephemeral X25519 public key    */
} kvm_pkt_pair_beacon_t;

typedef struct {
    kvm_hdr_t h;
    uint32_t  session;               /* echoed from beacon                */
    uint8_t   dev_pub[KVM_PUBKEY_LEN];
    uint16_t  fw_ver;
} kvm_pkt_pair_req_t;

/*
 * blob = AES-128-GCM(KEK, nonce, PMK[16] | slot | channel).
 * KEK and LMK are both HKDF-derived from the X25519 shared secret, so a
 * passive sniffer of the pairing exchange learns nothing usable.
 */
typedef struct {
    kvm_hdr_t h;
    uint32_t  session;
    uint8_t   nonce[KVM_NONCE_LEN];
    uint8_t   ct[KVM_PAIR_BLOB_LEN];
    uint8_t   tag[KVM_TAG_LEN];
} kvm_pkt_pair_confirm_t;

/*
 * Sent LMK-encrypted over ESP-NOW; `proof` = HKDF(shared, "espkvm done v1")
 * additionally proves at the application layer that the dongle really
 * completed the ECDH (defense in depth in case a stack ever delivers a
 * plaintext frame for an encrypted peer).
 */
typedef struct {
    kvm_hdr_t h;
    uint32_t  session;
    uint8_t   slot;
    uint8_t   proof[KVM_TAG_LEN];
} kvm_pkt_pair_done_t;

#pragma pack(pop)

_Static_assert(sizeof(kvm_hdr_t) == 12, "hdr size");
_Static_assert(sizeof(kvm_pkt_kbd_t) == 20, "kbd size");
_Static_assert(sizeof(kvm_pkt_mouse_t) == 19, "mouse size");
_Static_assert(sizeof(kvm_pkt_pair_confirm_t) == 12 + 4 + 12 + 18 + 16, "confirm size");

/* ---------------------------------------------------------------------- */
/* API                                                                    */
/* ---------------------------------------------------------------------- */

void kvm_hdr_fill(kvm_hdr_t *h, uint8_t type, uint32_t epoch, uint32_t seq);

/*
 * Validate a received buffer: magic, protocol version, and an *exact*
 * length match for the claimed type (every packet type is fixed-size, so
 * anything else is malformed or from a different protocol).
 * Returns the packet type (>0) or -1 if the packet must be dropped.
 */
int kvm_pkt_validate(const uint8_t *data, size_t len);

/* Expected total on-wire size for a type, or 0 if unknown type. */
size_t kvm_pkt_size(uint8_t type);

/* ---------------------------------------------------------------------- */
/* Anti-replay sliding window (RFC 6479 style, 64 packets wide)           */
/* ---------------------------------------------------------------------- */

typedef struct {
    bool     valid;      /* false until the first packet is accepted      */
    uint32_t epoch;      /* highest epoch seen                            */
    uint32_t seq;        /* highest seq seen within `epoch`               */
    uint64_t window;     /* bit i set => (seq - i) already seen           */
} kvm_replay_t;

void kvm_replay_reset(kvm_replay_t *r);

/*
 * Returns true (and updates state) iff the (epoch, seq) pair is fresh:
 *   - epoch older than the newest seen        -> reject
 *   - epoch newer                             -> accept, restart window
 *   - same epoch, seq newer                   -> accept, slide window
 *   - same epoch, seq within 64-wide window   -> accept once, then reject
 *   - same epoch, seq older than the window   -> reject
 */
bool kvm_replay_accept(kvm_replay_t *r, uint32_t epoch, uint32_t seq);

#ifdef __cplusplus
}
#endif
