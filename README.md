<div align="center">

<img src="docs/logo-256.png" alt="OctoKey" width="150">

# OctoKey

**One phone. Ten computers. One USB stick each.**
A wireless keyboard, mouse and macro runner for machines that have no
business sharing a keyboard — and, unlike the 2.4 GHz keyboard you already
own, actually encrypted.

[**⚡ Flash it from your browser →**](https://kbreddiee.github.io/octokey/) &nbsp;·&nbsp;
[Build guide](docs/BUILD.md) &nbsp;·&nbsp;
[Threat model](docs/SECURITY.md) &nbsp;·&nbsp;
[Protocol](common/PROTOCOL.md)

</div>

---

OctoKey is an open-source wireless KVM (keyboard/video/mouse — minus the
video) built from ESP32 sticks. Plug one into each machine; drive them all
from your phone's browser. The targets need no drivers and no software: each
stick enumerates as an ordinary USB keyboard + mouse, so BIOS screens,
locked-down POS terminals and headless servers all just work.

```
                       Wi-Fi                    ESP-NOW (AES)
   ┌────────┐   ┌──────────────┐   ┌──[stick]═▶ PC 1
   │ phone  │──▶│  hub stick   │──▶├──[stick]═▶ PC 2
   │browser │   │ (also a KVM  │   ├──[stick]═▶ PC 3
   └────────┘   │  into PC 0)  │   │    ...
                └──────┬───────┘   └──[stick]═▶ PC 9
                       ▼
                     PC 0
```

The hub is just another stick. Plug it into the machine that's always on —
it hosts the Wi-Fi network, serves the control panel, types into its *own*
computer over USB, and relays to the rest over encrypted ESP-NOW.

## Why it exists

- **10-port KVMs barely exist.** Commercial switches top out at 4–8 ports,
  cost hundreds, and want a cable octopus on your desk. A rack of lab
  machines, SBCs, POS terminals or servers deserves something cheaper.
- **Cheap wireless keyboards are a security dumpster fire.** MouseJack
  (2016) showed mainstream 2.4 GHz receivers accepting unencrypted keystroke
  injection from hundreds of meters away. OctoKey's radio link is
  AES-encrypted with per-stick keys from an ephemeral ECDH pairing, with
  replay protection. [The full threat model](docs/SECURITY.md) documents
  what it does *not* defend against, too.
- **Nothing to install on the targets.** If it takes a USB keyboard, it
  takes OctoKey.

## What you get

- **Phone control panel** — touchpad, on-screen keyboard, your phone's
  *native* keyboard (captured by input diffing, so Gboard swipe-typing
  works), and one-tap machine switching. Served straight off the stick at
  `http://192.168.4.1`; nothing installed, nothing in the cloud.
- **Script runner** — save named macros and fire them at a machine: literal
  text, key chords, and waits. Built for the multi-step console flows you'd
  otherwise type by hand at a terminal:

  ```
  key ctrl+alt+f2
  wait 2000
  type your-username
  enter
  type systemctl status myservice
  enter
  ```
- **Browser flashing** — the [web installer](docs/index.html) writes the
  firmware over WebSerial from desktop Chrome or Edge. No toolchain.
- **Pairing in two button presses** — no MAC-address copy-pasting, no config
  files. Tap Pair in the web UI, press BOOT on the new stick. Keys are
  derived fresh (X25519 → HKDF → AES) and persisted in NVS.
- **Status screen on the stick** — the T-Dongle-S3's LCD shows the 0–9 slot
  bar (green = active, red = paired, grey = empty), the Wi-Fi name and URL,
  and USB state.
- **Failsafes everywhere** — reboots return to the last active slot, sticks
  auto-release all keys if the link dies mid-keypress, and offline slots drop
  input instead of queueing stale keystrokes.

## Quickstart

1. **Get a [LILYGO T-Dongle-S3](https://www.lilygo.cc/products/t-dongle-s3)**
   (~$12) for each machine, plus one to act as the hub.
2. **Flash** — open the web installer in desktop Chrome/Edge, hold **BOOT**
   while plugging the stick in, and click Install. (Or
   `idf.py -p <port> flash` in `firmware/hub-dongle` with ESP-IDF v5.2.)
3. **Connect** — join the `octokey` Wi-Fi network (default password
   `octokey-air` — **change it**, see [SECURITY.md](docs/SECURITY.md)) and
   browse to `http://192.168.4.1`.
4. **Pair the rest** — flash the other sticks with `firmware/dongle`, tap
   **Pair** in the web UI, press **BOOT** on each stick.

## Hardware variants

The radio, crypto and pairing code is shared; pick the hub that suits you.

| Variant | Board | Best for |
|---|---|---|
| **`hub-dongle`** ⭐ | T-Dongle-S3 | One stick per machine, nothing else. The web installer ships this. |
| `hub-air` | any ESP32-S3 | Wi-Fi-only hub, phone is the keyboard. No USB host. |
| `hub` | ESP32-S3 DevKitC-1 | A *physical* keyboard plugged into the hub via USB host, OLED + encoder. |
| `hub-tdeck` | LILYGO T-Deck | Fully self-contained: onboard keyboard, trackball and screen. No phone. |
| `dongle` | ESP32-S2/S3 | The plain receiver that plugs into each target machine. |

## Repo layout

```
firmware/hub-dongle/  ⭐ one stick = USB HID to its own PC + Wi-Fi AP + web UI
                         + ESP-NOW hub for the other sticks
firmware/hub/         ESP32-S3: USB host HID → hotkey FSM → ESP-NOW TX
firmware/hub-air/     Wi-Fi-only hub (no USB host)
firmware/hub-tdeck/   LILYGO T-Deck: onboard keyboard + trackball + screen
firmware/dongle/      ESP-NOW RX → TinyUSB composite HID (kbd + mouse + media)
common/components/    kvm_hublink (ESP-NOW/pairing/store), kvm_webui (phone
                      web app), kvm_usbdev (TinyUSB HID), kvm_crypto, kvm_proto
common/PROTOCOL.md    versioned wire protocol
hardware/             fab-ready reference PCBs (hub + stick)
docs/                 the web installer (GitHub Pages) + BUILD / PAIRING / SECURITY
tests/host/           host-runnable unit tests (run in CI)
```

## FAQ

**What's the latency?** ESP-NOW adds ~1–2 ms. Switching machines is < 50 ms.
It feels wired.

**Range?** A desk, a rack, or a room with margin. Not a parking lot — that's
a feature, but don't rely on it; see the threat model.

**Mouse too?** Yes — buttons, 16-bit movement (high-DPI friendly), wheel,
horizontal scroll, and media keys.

**Does it do NKRO?** The link carries boot-style 6-key reports plus
modifiers. NKRO keyboards work; you just can't hold 7 letters at once across
the radio.

**Why not Bluetooth?** BLE HID needs pairing UI on every target machine and
does nothing for BIOS/POS boxes. A USB stick sidesteps all of it, and ESP-NOW
gives lower, more predictable latency.

**Video switching?** No — it's a KM, honestly. Pair it with a monitor that
has an input-select hotkey, or a cheap HDMI switch.

## Responsible use

OctoKey is a keyboard. It types into whatever it's plugged into, which is
exactly what makes it useful for administering machines you own — and
exactly why you should only ever plug it into those. Change the default
Wi-Fi password before using it anywhere shared: the phone↔hub hop is WPA2,
not the double-encrypted scheme used between sticks.
[SECURITY.md](docs/SECURITY.md) spells out the boundaries.

## License

[MIT](LICENSE). Dependencies are ESP-IDF built-ins plus two Espressif-official
managed components (`esp_tinyusb`, `usb_host_hid`).
