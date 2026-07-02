/* espkvm packet format helpers. Pure C — unit-tested on the host. */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include "kvm_proto.h"

void kvm_hdr_fill(kvm_hdr_t *h, uint8_t type, uint32_t epoch, uint32_t seq)
{
    h->magic   = KVM_MAGIC;
    h->version = KVM_PROTO_VERSION;
    h->type    = type;
    h->flags   = 0;
    h->epoch   = epoch;
    h->seq     = seq;
}

size_t kvm_pkt_size(uint8_t type)
{
    switch (type) {
    case KVM_PKT_KEYBOARD:     return sizeof(kvm_pkt_kbd_t);
    case KVM_PKT_MOUSE:        return sizeof(kvm_pkt_mouse_t);
    case KVM_PKT_CONSUMER:     return sizeof(kvm_pkt_consumer_t);
    case KVM_PKT_CONTROL:      return sizeof(kvm_pkt_control_t);
    case KVM_PKT_PAIR_BEACON:  return sizeof(kvm_pkt_pair_beacon_t);
    case KVM_PKT_PAIR_REQ:     return sizeof(kvm_pkt_pair_req_t);
    case KVM_PKT_PAIR_CONFIRM: return sizeof(kvm_pkt_pair_confirm_t);
    case KVM_PKT_PAIR_DONE:    return sizeof(kvm_pkt_pair_done_t);
    default:                   return 0;
    }
}

int kvm_pkt_validate(const uint8_t *data, size_t len)
{
    if (data == NULL || len < sizeof(kvm_hdr_t)) {
        return -1;
    }

    kvm_hdr_t h;
    memcpy(&h, data, sizeof(h));   /* avoid unaligned access on the buffer */

    if (h.magic != KVM_MAGIC || h.version != KVM_PROTO_VERSION) {
        return -1;
    }

    size_t want = kvm_pkt_size(h.type);
    if (want == 0 || len != want) {
        /* Unknown type or truncated/oversized frame: drop. Exact-size
         * matching doubles as a bounds check for every field access the
         * caller will do after casting. */
        return -1;
    }
    return (int)h.type;
}

void kvm_replay_reset(kvm_replay_t *r)
{
    memset(r, 0, sizeof(*r));
}

bool kvm_replay_accept(kvm_replay_t *r, uint32_t epoch, uint32_t seq)
{
    if (!r->valid) {
        r->valid  = true;
        r->epoch  = epoch;
        r->seq    = seq;
        r->window = 1;             /* bit 0 = `seq` itself */
        return true;
    }

    if (epoch < r->epoch) {
        return false;              /* stale hub boot — classic replay */
    }

    if (epoch > r->epoch) {
        /* Hub rebooted: fresh epoch, restart the window. */
        r->epoch  = epoch;
        r->seq    = seq;
        r->window = 1;
        return true;
    }

    /* Same epoch. */
    if (seq > r->seq) {
        uint32_t shift = seq - r->seq;
        r->window = (shift >= 64) ? 0 : (r->window << shift);
        r->window |= 1;
        r->seq = seq;
        return true;
    }

    uint32_t behind = r->seq - seq;
    if (behind >= 64) {
        return false;              /* too old to track — reject */
    }
    uint64_t bit = 1ULL << behind;
    if (r->window & bit) {
        return false;              /* duplicate */
    }
    r->window |= bit;
    return true;
}
