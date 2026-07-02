# espkvm

**One keyboard. Ten computers. ~$50. No drivers, no software on the
targets, and — unlike the 2.4 GHz keyboard you already own — actually
encrypted.**

espkvm is an open-source wireless KVM (keyboard/video/mouse — minus the
video) built from ESP32 dev boards. A hub with your keyboard plugged into
it beams input to tiny USB dongles on up to **10 machines**, switching
between them instantly with a rotary knob or a keyboard chord.

```
                          ┌────────────┐
   your keyboard ──USB──▶ │    hub     │      ┌─[dongle]═▶ PC 0
   (or its 2.4GHz         │ ESP32-S3   │ ESP  ├─[dongle]═▶ PC 1
    receiver dongle)      │ OLED+knob  │─NOW─▶├─[dongle]═▶ PC 2
                          └────────────┘ AES  │    ...
                                              └─[dongle]═▶ PC 9
```

## Why this exists

- **10-port KVMs barely exist.** Commercial KVM switches top out at 4–8
  ports, cost hundreds, and want a cable octopus on your desk. A rack of
  lab machines, SBCs, POS terminals or servers wants something cheaper.
- **Cheap wireless keyboards are a security dumpster fire.** MouseJack
  (2016) showed mainstream 2.4 GHz receivers accepting unencrypted
  keystroke injection from hundreds of meters. espkvm's radio link is
  AES-encrypted with per-dongle keys from an ephemeral ECDH pairing, with
  replay protection. [The full threat model is documented](docs/SECURITY.md),
  including what it *doesn't* defend against.
- **The targets need nothing installed.** Each dongle enumerates as a bog
  standard USB HID keyboard + mouse + media keys. BIOS setup screens,
  Windows, macOS, Linux, locked-down POS terminals — if it takes a USB
  keyboard, it works.

## Demo

*(placeholder — GIF of knob-switching between three machines goes here)*

## What you get

- **Hub** (ESP32-S3 DevKitC-1, ~$9): USB host for your keyboard *or* your
  wireless keyboard's receiver — composite devices like the Rii X8's
  dongle (keyboard + touchpad + media keys) are parsed generically from
  their HID report descriptors, not hardcoded.
- **Dongles** (ESP32-S2 Mini, ~$3.50 each): plug into each target machine.
- **Switching**: rotate + click the encoder, or double-tap **Right-Ctrl**
  then a digit `0–9` from the keyboard itself. Sub-50 ms switches, with a
  *release-all* sent to the machine you're leaving so nothing sticks.
- **OLED status**: active slot name/number, per-slot online/offline (from
  real radio ACKs), pairing UI.
- **Pairing in two button presses** — no MAC address copy-pasting, no
  config files. Hold the hub knob 3 s, tap BOOT on a dongle, done. Keys
  are derived fresh (X25519 → HKDF → AES) and persisted in NVS.
- **Failsafes everywhere**: hub reboot returns to the last active slot;
  dongles auto-release all keys if the link dies mid-keypress; offline
  slots drop input instead of queueing stale keystrokes.

## Quickstart (15 minutes + soldering four wires)

1. **Order parts** — full BOM with search terms in
   [docs/BUILD.md](docs/BUILD.md). Hub ≈ $15, each dongle ≈ $3.50.
2. **Wire the hub**: OLED on GPIO8/9, encoder on GPIO4/5/6, a USB-A
   breakout on GPIO19/20 + 5V for the keyboard. One diagram, seven wires.
3. **Flash**: `idf.py build flash` in `firmware/hub` and
   `firmware/dongle` (ESP-IDF v5.2), or use the ESP Web Tools images from
   releases — no toolchain needed.
4. **Pair**: hold the knob 3 s, press BOOT on each dongle.
5. Double-tap Right-Ctrl, hit `2`, and you're typing on machine 2.

## FAQ

**What's the latency?** ESP-NOW adds ~1–2 ms on top of the keyboard's own
USB polling. Switching is < 50 ms. It feels wired.

**Range?** Standard ESP32 Wi-Fi radio: a desk, a rack, or a room with
margin. Not a parking lot (that's a feature, but don't rely on it — see
the threat model).

**Does it do NKRO?** The link carries boot-style 6-key reports (plus
modifiers). NKRO keyboards work; you just can't hold 7 letters at once
across the radio.

**Mouse too?** Yes — buttons, movement (16-bit, high-DPI friendly), wheel,
horizontal scroll, and media keys. A keyboard-with-touchpad receiver is
the intended happy path.

**Why not Bluetooth?** BLE HID needs pairing UI on every target machine
and does nothing for BIOS/POS boxes. A USB dongle sidesteps all of it, and
ESP-NOW gives lower, more predictable latency than BLE.

**Why not just buy a multi-device keyboard?** Logitech Flow & friends top
out at 3 devices, need drivers/cloud software, and their RF story is,
uh, historical. espkvm does 10, driverless, with documented crypto.

**Can I rename "PC 3"?** Slot names live in the hub's NVS; a serial
console rename command is on the roadmap. PRs welcome.

**Video switching?** No — it's a KM, honestly. Pair it with a monitor
that has an input-select hotkey, or a cheap HDMI switch.

## Repo layout

```
firmware/hub/     ESP32-S3: USB host HID → hotkey FSM → ESP-NOW TX, OLED+encoder UI
firmware/dongle/  ESP32-S2: ESP-NOW RX → TinyUSB composite HID (kbd+mouse+media)
common/           wire protocol (versioned, documented), pairing crypto, HID
                  report-descriptor parser — all unit-tested on the host
tools/            ESP Web Tools manifest generator + flasher page
docs/             BUILD (BOM/wiring/flash), PAIRING (user guide), SECURITY (threat model)
tests/host/       host-runnable unit tests (run in CI)
```

Protocol documentation: [common/PROTOCOL.md](common/PROTOCOL.md).
CI builds both firmwares (ESP-IDF v5.2) and runs the host tests on every
push; tagged releases ship ready-to-flash merged images.

## License

[MIT](LICENSE). Dependencies: ESP-IDF built-ins plus two Espressif-official
managed components (`esp_tinyusb` on the dongle, `usb_host_hid` on the hub).
