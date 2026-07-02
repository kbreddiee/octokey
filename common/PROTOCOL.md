# espkvm wire protocol (v1)

Authoritative definition lives in [`components/kvm_proto/include/kvm_proto.h`](components/kvm_proto/include/kvm_proto.h).
This document explains the design. All multi-byte fields are **little-endian**,
all structs are packed. Transport is **ESP-NOW** on a fixed Wi-Fi channel
(default 1, `menuconfig → espkvm`). Max ESP-NOW payload is 250 bytes; every
espkvm packet is far below that and **fixed-size per type** — receivers drop
anything whose length doesn't exactly match its claimed type.

## Header (12 bytes, prefixes every packet)

| Offset | Size | Field     | Meaning                                        |
|-------:|-----:|-----------|------------------------------------------------|
| 0      | 1    | `magic`   | `0xA7`                                          |
| 1      | 1    | `version` | `0x01` — receivers drop mismatches              |
| 2      | 1    | `type`    | see below                                       |
| 3      | 1    | `flags`   | reserved, must be 0                             |
| 4      | 4    | `epoch`   | hub boot counter (persisted in NVS)             |
| 8      | 4    | `seq`     | monotonic per epoch                             |

## Packet types

| Type   | Name           | Dir           | Encrypted? | Payload |
|--------|----------------|---------------|------------|---------|
| `0x01` | `KEYBOARD`     | hub → dongle  | ✅ CCMP/LMK | `mods u8, reserved u8, keys[6] u8` (boot-shaped, 6KRO) |
| `0x02` | `MOUSE`        | hub → dongle  | ✅ CCMP/LMK | `buttons u8, dx i16, dy i16, wheel i8, pan i8` |
| `0x03` | `CONSUMER`     | hub → dongle  | ✅ CCMP/LMK | `usage u16` (Consumer-page usage held; `0` = released) |
| `0x04` | `CONTROL`      | hub → dongle  | ✅ CCMP/LMK | `op u8`: `1` RELEASE_ALL, `2` PING, `3` UNPAIR |
| `0x10` | `PAIR_BEACON`  | hub → bcast   | ❌ plaintext | `session u32, hub_pub[32]` (ephemeral X25519) |
| `0x11` | `PAIR_REQ`     | dongle → hub  | ❌ plaintext | `session u32, dev_pub[32], fw_ver u16` |
| `0x12` | `PAIR_CONFIRM` | hub → dongle  | ❌ plaintext (AEAD inside) | `session u32, nonce[12], ct[18], tag[16]` |
| `0x13` | `PAIR_DONE`    | dongle → hub  | ✅ CCMP/LMK | `session u32, slot u8, proof[16]` |

**Nothing typed on the keyboard ever leaves the hub in plaintext.** The only
plaintext packets are the pairing handshake, which carries ephemeral public
keys and an AEAD ciphertext — useless to a sniffer.

## Pairing handshake

```
hub (encoder held 3 s)                    dongle (BOOT pressed)
──────────────────────                    ─────────────────────
gen ephemeral X25519 (h_priv, h_pub)      gen ephemeral X25519 (d_priv, d_pub)
session = random u32

  PAIR_BEACON {session, h_pub}  ──── broadcast every 300 ms ────▶

  ◀──────────────────  PAIR_REQ {session, d_pub, fw_ver}

shared = X25519(h_priv, d_pub)            shared = X25519(d_priv, h_pub)
KEK  = HKDF(shared, "espkvm kek v1")      (same derivations)
LMK  = HKDF(shared, "espkvm lmk v1")
PROOF= HKDF(shared, "espkvm done v1")
slot = existing MAC's slot, else lowest free

  PAIR_CONFIRM {session, nonce,
     AES-128-GCM(KEK, nonce,
        PMK[16] | slot | channel)} ──────────────────▶
                                          decrypt blob; persist
register dongle as ESP-NOW                {hub_mac, PMK, LMK, slot} in NVS;
encrypted peer (LMK)                      register hub as encrypted peer

  ◀───── PAIR_DONE {session, slot, PROOF}   (CCMP-encrypted with LMK)

verify PROOF; persist pairing in NVS; done.
```

- The **PMK** is generated randomly on the hub at first boot and shared with
  every dongle inside the GCM blob (ESP-NOW uses it to wrap LMKs; both sides
  of a link must set the same PMK).
- Keys are **ephemeral per pairing-mode entry**; a passive sniffer of the
  handshake learns nothing (see `docs/SECURITY.md` for the active-MITM
  caveat during the pairing window).
- If `PAIR_DONE` doesn't arrive within 5 s the hub rolls back and keeps
  beaconing; re-press BOOT on the dongle.

## Replay protection

ESP-NOW's CCMP encrypts and authenticates frames but does not guarantee
replay rejection across reboots, so espkvm layers its own:

1. `epoch` — the hub increments a persisted boot counter every startup.
   Dongles persist the highest epoch seen and reject anything older, so
   captured ciphertext from before a hub reboot is dead forever.
2. `seq` — monotonic within an epoch. Dongles keep an RFC-6479-style 64-bit
   sliding window per hub: newer → slide; within window → accept once;
   older/duplicate → drop.

Pairing packets use `epoch = seq = 0`; their freshness comes from the random
`session` id and the ephemeral keys.

## Versioning policy

`version` is bumped on any incompatible change to the header or payloads.
Receivers **must** drop unknown versions/types (`kvm_pkt_validate()` enforces
magic, version, known type, and exact length in one call — every field access
after it is bounds-safe by construction).
