/* Host unit tests: packet validation + anti-replay window. */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "kvm_proto.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void test_validate(void)
{
    kvm_pkt_kbd_t k;
    kvm_hdr_fill(&k.h, KVM_PKT_KEYBOARD, 7, 42);
    k.mods = 0x02;
    k.reserved = 0;
    memset(k.keys, 0, sizeof(k.keys));

    CHECK(kvm_pkt_validate((uint8_t *)&k, sizeof(k)) == KVM_PKT_KEYBOARD);

    /* Wrong length for the claimed type */
    CHECK(kvm_pkt_validate((uint8_t *)&k, sizeof(k) - 1) == -1);
    CHECK(kvm_pkt_validate((uint8_t *)&k, sizeof(k) + 1) == -1);

    /* Bad magic / version / type */
    kvm_pkt_kbd_t bad = k;
    bad.h.magic = 0x00;
    CHECK(kvm_pkt_validate((uint8_t *)&bad, sizeof(bad)) == -1);
    bad = k;
    bad.h.version = KVM_PROTO_VERSION + 1;
    CHECK(kvm_pkt_validate((uint8_t *)&bad, sizeof(bad)) == -1);
    bad = k;
    bad.h.type = 0x7F;
    CHECK(kvm_pkt_validate((uint8_t *)&bad, sizeof(bad)) == -1);

    /* Runt frames */
    CHECK(kvm_pkt_validate((uint8_t *)&k, 3) == -1);
    CHECK(kvm_pkt_validate(NULL, sizeof(k)) == -1);
}

static void test_replay(void)
{
    kvm_replay_t r;
    kvm_replay_reset(&r);

    /* First packet always accepted */
    CHECK(kvm_replay_accept(&r, 1, 100));
    /* Exact duplicate rejected */
    CHECK(!kvm_replay_accept(&r, 1, 100));
    /* Forward progress */
    CHECK(kvm_replay_accept(&r, 1, 101));
    CHECK(kvm_replay_accept(&r, 1, 105));
    /* Out-of-order within window accepted once */
    CHECK(kvm_replay_accept(&r, 1, 103));
    CHECK(!kvm_replay_accept(&r, 1, 103));
    /* Too old (window is 64 wide) */
    CHECK(kvm_replay_accept(&r, 1, 200));
    CHECK(!kvm_replay_accept(&r, 1, 136));   /* 200-136=64 => outside */
    CHECK(kvm_replay_accept(&r, 1, 137));    /* 63 behind => inside   */

    /* Older epoch always rejected (hub reboot replay) */
    CHECK(!kvm_replay_accept(&r, 0, 999999));
    /* Newer epoch resets the window */
    CHECK(kvm_replay_accept(&r, 2, 1));
    CHECK(!kvm_replay_accept(&r, 1, 201));
    CHECK(!kvm_replay_accept(&r, 2, 1));
    CHECK(kvm_replay_accept(&r, 2, 2));

    /* Huge jump forward keeps working */
    CHECK(kvm_replay_accept(&r, 2, 1000000));
    CHECK(!kvm_replay_accept(&r, 2, 999000));
}

int main(void)
{
    test_validate();
    test_replay();
    if (failures) {
        printf("test_proto: %d FAILURES\n", failures);
        return 1;
    }
    printf("test_proto: all ok\n");
    return 0;
}
