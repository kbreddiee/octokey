# espkvm security model

Wireless keyboards have a bad history. The MouseJack family of
vulnerabilities (Bastille, 2016) showed that many cheap 2.4 GHz keyboard
receivers accept **unencrypted, unauthenticated** injection — an attacker
with a $30 dongle types on your machine from across the parking lot, and
some of them let you sniff keystrokes too. espkvm is a 2.4 GHz keyboard
link as well, so it has to answer the same questions honestly.

## TL;DR

- Every keystroke, mouse move and media key is **AES-CCMP encrypted**
  (ESP-NOW encrypted peers) with a **unique per-dongle key**.
- Keys are derived via **ephemeral X25519 ECDH** at pairing time; nothing
  secret is ever broadcast.
- Every packet carries an **epoch + sequence number**; receivers enforce a
  sliding replay window that survives reboots. Captured traffic cannot be
  replayed later.
- Unknown senders, malformed frames, wrong protocol versions and unpaired
  MACs are dropped before any state is touched.

## What goes over the air

| Traffic | Protection |
|---------|-----------|
| Keystrokes / mouse / media / control | CCMP encryption + authentication with the per-dongle LMK, plus app-level epoch/seq anti-replay |
| Pairing beacon + request | Plaintext, but contains only random session IDs and *ephemeral public keys* |
| Pairing confirm | Plaintext frame carrying an AES-128-GCM blob (the hub's PMK + slot), keyed from the ECDH shared secret |
| Pairing done | CCMP-encrypted with the just-derived LMK + HKDF possession proof |

A passive sniffer parked on the channel forever learns: that an espkvm
exists, its MAC addresses, when you type (traffic analysis), and nothing
else. Contrast with MouseJack-class receivers where the same sniffer gets
your keystrokes.

## Key hierarchy

```
pairing:  X25519(ephemeral hub key, ephemeral dongle key) = shared
          KEK   = HKDF-SHA256(shared, "espkvm kek v1")   — wraps the PMK blob
          LMK   = HKDF-SHA256(shared, "espkvm lmk v1")   — per-dongle ESP-NOW key
          PROOF = HKDF-SHA256(shared, "espkvm done v1")  — pairing completion proof

hub NVS:  PMK (random, generated once) + per-slot {MAC, LMK, name}
dongle:   {hub MAC, PMK, LMK, slot} + highest hub epoch seen
```

ESP-NOW's model: one global PMK per device, one LMK per encrypted peer;
frames are CCMP-protected using the LMK (the PMK encrypts the LMKs
internally). Because each dongle's LMK comes from an independent ECDH,
**compromising one dongle does not decrypt traffic to any other** — and
traffic to dongle A is not even decryptable by dongle B in real time.

## Replay protection

CCMP stops forgery but ESP-NOW does not guarantee replay rejection across
reboots, so espkvm adds its own layer (`kvm_proto.c`, unit-tested):

- The hub persists a **boot epoch** counter; every packet carries
  `(epoch, seq)`.
- Dongles keep an RFC-6479-style 64-packet sliding window per epoch,
  reject any older epoch, and **persist the highest epoch to NVS** — so
  frames captured before a hub reboot are permanently dead.

Residual window: for a few packets right after a *dongle* (not hub) reboot
within an unchanged hub epoch, a recorded frame could theoretically be
replayed once. Consequence: a repeat of old keystrokes on a machine the
attacker is already physically near; dongles reboot only when unplugged.
Accepted and documented rather than papered over.

## Pairing-window trust

Pairing is TOFU (trust on first use) protected by physical presence: it
requires simultaneously holding the hub's encoder button and pressing the
dongle's BOOT button, within a 30 s window, and the hub pairs exactly one
device per handshake. An active attacker present *during that window*
could in principle race the legitimate dongle (classic unauthenticated-DH
MITM). If your threat model includes attackers with radios inside your
office during the 30 seconds you pair, do the pairing somewhere else —
after pairing, the link is pinned to the stored keys and MACs, and there
is no re-negotiation to attack.

## espkvm Air: the phone↔hub hop

`firmware/hub-air` (phone browser as keyboard/touchpad — no physical
keyboard) adds a second radio hop that the rest of this document doesn't
cover: **phone → hub**, over the hub's own Wi-Fi access point, *before*
anything reaches the hub↔dongle ESP-NOW link described above. That second
leg (hub→dongle) is unchanged — same ephemeral-ECDH pairing, same unique
per-dongle keys, same replay protection. The new leg is not:

- **Confidentiality is exactly WPA2-PSK.** The phone↔hub hop is secured
  by the Wi-Fi password you set (`CONFIG_ESPKVM_AP_PASSWORD`), full stop —
  there is no additional per-session key exchange on top of it the way
  there is for hub↔dongle. **Change the default password** (`octokey-air`)
  before relying on this anywhere you don't fully trust the room; treat it
  like a shared house key, not a cryptographic secret.
- **Anyone with the password can type as you.** Up to
  `CONFIG_ESPKVM_AP_MAX_CLIENTS` phones can hold the WebSocket open at
  once; there's no per-user identity, no PIN prompt, no session approval
  on the hub. Rotate the password if a phone that had it is no longer
  trusted.
- **The control channel itself is plaintext-ish (ws://, not wss://).**
  ESP-IDF's `esp_http_server` doesn't do TLS termination lightly on these
  chips (and a self-signed cert would just train users to click through
  browser warnings — worse for security, not better), so traffic between
  the phone and the hub is WPA2-encrypted at the Wi-Fi layer only, not
  additionally at the application layer. On the hub's own AP this is the
  same protection your keystrokes get on literally any home Wi-Fi router;
  it is *not* the same guarantee as the double-encrypted, replay-protected
  hub↔dongle link.
- **No physical-presence pairing gate.** Unlike hub↔dongle pairing (which
  needs someone's thumb on the dongle's BOOT button), joining the phone
  Wi-Fi network only needs the password. If that's not the trust model you
  want, use a hub variant with a physical keyboard instead — the rest of
  espkvm's guarantees (documented above) are identical either way.

If your threat model is "attacker in Wi-Fi range of my desk," espkvm Air
is not the variant to reach for. If it's "convenient input for a home
lab / demo bench / a room you already control," it's a reasonable and
honestly-documented trade for the convenience of not needing a physical
keyboard at all.

## What espkvm does NOT defend against

- **RF jamming / DoS.** 2.4 GHz is a shared band; anyone can shout over
  it. Fail-safe: dongles auto-release all keys when the link dies, and the
  hub UI shows the slot offline.
- **Physical theft of a paired dongle.** It holds its keys in plain NVS
  (neither chip ships with flash encryption enabled here). A stolen dongle
  can *receive* what the hub sends it — it still cannot impersonate other
  dongles. Unpair stolen hardware from the hub menu. Enabling IDF flash
  encryption + secure boot is possible on both targets if you need it.
- **A compromised target machine.** The dongle is a keyboard; it types
  what the hub says. It cannot exfiltrate anything from the host (it has
  no IN traffic besides standard HID status), but the host sees whatever
  you type while switched to it, obviously.
- **Traffic analysis.** Packet timing correlates with typing cadence.
  Encrypted lengths are constant per report type.

## Defensive-coding notes

Every frame from the radio passes `kvm_pkt_validate()` — magic, protocol
version, known type, and an **exact** length match — before any field is
read, then a source-MAC allowlist, then replay. Buffers are fixed-size and
copied out of the Wi-Fi callback context. The USB side bounds-checks HID
report descriptors the same way (`kvm_hidparse.c`, fuzz-friendly and
unit-tested on the host).

Found something? Open a GitHub security advisory or issue. This is a
hobby project, but crypto bugs get fixed with priority.
