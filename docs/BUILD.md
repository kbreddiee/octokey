# Building espkvm — parts, wiring, flashing

## Bill of materials

Prices are typical mid-2026 street prices. Both AliExpress (cheap, 1–3 weeks)
and Amazon (2× the price, tomorrow) carry all of these — search the exact
terms below.

**Hub, pick one:**

- **espkvm Air** — no dev board, no wiring, **no physical keyboard at
  all**: any bare ESP32/S2/S3/C3 (~$3–5) hosts a Wi-Fi network and your
  phone's browser becomes the keyboard, touchpad and switcher. See
  [espkvm Air](#espkvm-air-phone-as-the-hub-no-usb-keyboard) below —
  skip straight there if this is your build.
- **LILYGO T-Deck / T-Deck Plus** — no phone, no PC, no external
  keyboard: a self-contained onboard keyboard + trackball + screen
  (~$65–90, real hardware limitations — read before ordering). See
  [espkvm hub-tdeck](#espkvm-hub-tdeck-fully-self-contained-no-phonepc-needed).
- **Custom espkvm hub PCB** — button + 10 slot LEDs, 18650 battery slot,
  built-in USB-A keyboard port; full fab-ready reference design in
  [`hardware/hub-refdesign/`](../hardware/hub-refdesign/README.md)
  (~$10–14/board assembled, 1–2 week fab lead time)
- LILYGO T-Embed (option A below — ready-made, cased, screen + knob,
  ~$29 + $8 splitter cable)
- DIY DevKitC-1 stack (option B — cheapest, all parts exposed, and the
  recommended bring-up board while custom PCBs are at the fab)

The BOM below is for a **physical-keyboard hub** (custom PCB / T-Embed /
DevKitC-1). If you're building espkvm Air, skip to its section — all you
need is one ESP32 board and dongles.

| # | Part | Search term | Qty | ~AliExpress | ~Amazon |
|---|------|-------------|-----|------------:|--------:|
| 0 | LILYGO T-Embed (hub option A) | `LILYGO T-Embed ESP32-S3` | 1 | $28 | $29 |
| 1 | ESP32-S3 DevKitC-1 (hub option B) | `ESP32-S3-DevKitC-1 N8R2` | 1 | $8 | $14 |
| 2 | ESP32-S2 Mini (Lolin/Wemos) | `S2 Mini ESP32-S2 Lolin` | 1 per target PC (up to 10) | $3.50 | $6 |
| 3 | SSD1306 0.96" OLED, 128×64, I²C | `SSD1306 0.96 I2C OLED` | 1 | $2.50 | $5 |
| 4 | EC11 rotary encoder module (breakout w/ pins) | `KY-040 rotary encoder` | 1 | $1.50 | $4 |
| 5 | USB-A female breakout board | `USB A female breakout DIP` | 1 | $1 | $3 |
| 6 | Dupont jumper wires (F-F, 10 cm) | `dupont jumper female` | 1 set | $1.50 | $4 |
| 7 | (optional) USB-C→A adapters for dongles on USB-A-only PCs | `USB C female to USB A male adapter` | as needed | $1 | $2 |

**Totals:** hub ≈ **$15**; each dongle ≈ **$3.50**. A 4-machine setup lands
around **$30**, a full 10-machine build around **$50** (AliExpress pricing).

Also useful: a USB 5 V/2 A phone charger to power the hub, and a data-capable
(not charge-only!) USB cable for flashing.

### Dongle form-factor options

The S2 Mini needs a cable or adapter to reach a USB port. If you want
thumb-drive-style dongles that plug straight in:

| Option | Price | Notes |
|--------|------:|-------|
| **LILYGO T-Dongle-S3** (ESP32-S3, USB-A plug, 160×80 LCD) | ~$12 | recommended: the LCD shows the slot number + ACTIVE state; all pin defaults match — just `idf.py set-target esp32s3` |
| S2 Mini + rigid `USB-C female to USB-A male adapter` | +$1 | cheapest; stock firmware |
| M5Stack AtomS3U (ESP32-S3, USB-A plug, cased) | ~$13 | set `ESPKVM_BTN_GPIO=41`; no status display |
| Custom PCB | ~$6–9/pc @ qty 10 | full reference design in [`hardware/dongle-refdesign/`](../hardware/dongle-refdesign/README.md) |

CI ships both `espkvm-dongle` (ESP32-S2) and `espkvm-dongle-s3` (ESP32-S3)
images. The S3 image has the T-Dongle-S3's LCD enabled by default
(`ESPKVM_LCD`, harmless on panel-less boards): a huge slot digit, a green
**ACTIVE** screen when that machine is the selected target, and hub-offline
/ USB warnings. On boards whose only LED is addressable RGB, the plain
status LED is disabled (`ESPKVM_LED_GPIO=-1`, the S3 default).

## espkvm Air: phone as the hub, no USB keyboard

`firmware/hub-air` is a different hub entirely: it has no USB host, no
display, no encoder — it's a radio bridge. It hosts a small Wi-Fi network
and a phone browser becomes the keyboard, touchpad and switcher. Runs on
whatever bare ESP32 you have lying around; **no wiring, no case, no BOM
beyond the board itself.**

```sh
cd firmware/hub-air
idf.py set-target esp32s3      # or esp32, esp32c3, esp32s2 — any Wi-Fi chip
idf.py build flash
```

(or flash `espkvm-hub-air-web.bin` from the web flasher).

1. Power the board (any USB source — a phone charger is fine).
2. On your phone, join the Wi-Fi network **`espkvm`** (password
   `espkvm-air` by default — **change both** in `idf.py menuconfig →
   espkvm hub-air` before you trust this anywhere; see
   [SECURITY.md](SECURITY.md#espkvm-air-the-phonehub-hop)).
3. Open **`http://192.168.4.1/`**. Three tabs: **Devices** (pair new
   dongles, tap a slot to switch), **Touchpad** (drag to move, tap to
   click, two fingers to scroll/right-click), **Keyboard** (full QWERTY
   with Ctrl/Alt/Win/Shift as toggles, arrows, media keys).

Pairing dongles works exactly as elsewhere — tap "Pair a new dongle" in
the Devices tab (same 30 s ESP-NOW beacon window as every other hub),
then press BOOT on the dongle. Everything downstream of pairing (the
dongle firmware, the ESP-NOW protocol, replay protection) is identical to
every other hub variant; only the "top half" — where keystrokes originate
— is different.

Because there's no USB host, espkvm Air can't provision dongles over USB
(no `flasher.c` on this variant) — flash new dongles from a PC or another
hub instead.

## espkvm hub-tdeck: fully self-contained, no phone/PC needed

The [LILYGO T-Deck / T-Deck Plus](https://lilygo.cc/) (~$65–90, ESP32-S3,
2.8" screen, physical keyboard, trackball) is the most self-contained
option: unlike espkvm Air it needs no phone, and unlike every other hub
it needs no external keyboard either. Build:

```sh
cd firmware/hub-tdeck
idf.py build flash
```

**Important limitation, read before ordering:** the onboard keyboard is a
35-key BlackBerry-style matrix — letters, two Shift keys, Space, Enter,
Backspace, a Symbol layer (numbers/punctuation), and a Mic key. **There
is no physical Ctrl, Alt, Win, arrows, Esc, Tab, or F-keys.** This is a
hardware fact about the board, not a firmware gap — `firmware/hub-tdeck`
works around it as best it can:

| Physical key | What it sends |
|---|---|
| Letters / Space / Enter / Backspace / Symbol layer | themselves, as normal typing |
| **Alt** (held) | **Ctrl** — Alt+C/V/Z/A/F reach the target as real Ctrl shortcuts |
| **double-tap Alt, then a digit** | switch slots (same trick as double-Right-Ctrl on the other hubs) |
| Trackball roll | mouse movement |
| **Symbol held + trackball roll** | arrow keys (partial fix for the missing arrow keys) |
| Trackball center click | short = left-click, hold 3 s = pairing mode, triple-click + one more click = forget the active slot |

If you need real Alt-Tab, Esc, Tab, or function-key shortcuts on the
target machine, this isn't the hub for that — pick the T-Embed, the
custom PCB, or espkvm Air instead. If your workflow is mostly typing plus
Ctrl+C/V/Z/A/F and mouse movement, it's a genuinely nice all-in-one.

Also note: the "Plus" variant's GPS and LoRa radio are unused by
espkvm — the plain **T-Deck** (no Plus, cheaper) has the identical
keyboard/trackball/screen/ESP32-S3 this firmware needs.

## Hub option A: LILYGO T-Embed (zero wiring)

The [LILYGO T-Embed](https://lilygo.cc/) (~$29) is an ESP32-S3 in a finished
enclosure with a 320×170 ST7789 LCD, a rotary encoder with push-button and
a LiPo charge circuit — everything the hub needs, no soldering. Build with:

```sh
cd firmware/hub
idf.py set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.tembed" build flash
```

(or flash the `espkvm-hub-tembed-web.bin` release image).

Two accessories complete it:

- **USB-C OTG splitter with power injection** (search `USB C OTG splitter
  power`, ~$8 — the kind sold for Fire TV Sticks): its C-plug goes into the
  T-Embed, the keyboard/receiver into its USB-A socket, and a 5 V charger
  into its power leg. This powers the board, charges the battery *and*
  feeds the keyboard at the same time, through the single USB-C port.
- **(optional) 1S LiPo with PH2.0 plug** (~$5, e.g. 1000 mAh): plugs into
  the battery connector for cable-free operation between charges.

Controls are identical to the DevKit build (rotate/click/double-click/hold);
the UI renders 2× scaled on the color panel.

## Hub option B: ESP32-S3 DevKitC-1 (breadboard-style, ~$15)

All pins are configurable via `idf.py menuconfig → espkvm hub`; these are
the defaults:

| DevKitC-1 pin | Goes to |
|---------------|---------|
| `3V3`         | OLED VCC, encoder `+`/VCC (if your module has one) |
| `GND`         | OLED GND, encoder GND |
| `GPIO8`       | OLED SDA |
| `GPIO9`       | OLED SCL |
| `GPIO4`       | Encoder A (CLK) |
| `GPIO5`       | Encoder B (DT) |
| `GPIO6`       | Encoder switch (SW) |

The encoder pins use internal pull-ups; bare EC11s (no module) wire directly:
common pin → GND, A/B/SW → the GPIOs.

### Keyboard port (the one gotcha on this board)

The DevKitC-1's **USB-OTG connector does not supply 5 V to a downstream
device** out of the box (its VBUS diode only feeds power *in*). The clean,
no-soldering-on-the-devkit fix is the USB-A breakout from the BOM:

| USB-A breakout | DevKitC-1 |
|----------------|-----------|
| VBUS (5V)      | `5V` pin  |
| D−             | `GPIO19`  |
| D+             | `GPIO20`  |
| GND            | `GND`     |

Power the board through the **UART** USB port (that's also your log
console), plug the keyboard or its 2.4 GHz receiver into the breakout.
GPIO19/20 are the same pins as the onboard OTG connector — just don't use
both at once. Keep the D+/D− wires short (< 10 cm).

*Alternative for advanced users:* bridge the VBUS Schottky diode next to
the OTG connector with a solder blob, then the onboard OTG port powers
devices directly (board-revision dependent — check with a multimeter).

## Wiring the dongles

None. The S2 Mini plugs straight into the target machine's USB port. The
onboard LED (GPIO15) and BOOT button (GPIO0) are all the UI it has.

## Flashing

### Option 1: ESP-IDF (development)

Install [ESP-IDF v5.2](https://docs.espressif.com/projects/esp-idf/en/v5.2.2/esp32s3/get-started/index.html), then:

```sh
# hub — plug the DevKitC-1's UART port into your PC
cd firmware/hub
idf.py set-target esp32s3
idf.py build flash monitor

# dongle — S2 Mini must be in download mode the first time:
#   hold "0" (BOOT), tap "RST", release "0"
cd firmware/dongle
idf.py set-target esp32s2
idf.py build flash
```

After the first flash the S2 Mini re-enters download mode automatically on
`idf.py flash`. Dongle logs are on UART0 (GPIO43 TX) if you ever need them;
the USB port is fully occupied being a keyboard.

### Option 2: Web flasher (no toolchain)

Every release ships merged images (`espkvm-hub-web.bin`,
`espkvm-dongle-web.bin`) plus ESP Web Tools manifests, generated by
`tools/gen_manifest.py`. Serve `tools/webflash/` together with those files
and flash from Chrome/Edge — or use `esptool.py` directly:

```sh
esptool.py --chip esp32s3 write_flash 0x0 espkvm-hub-web.bin
esptool.py --chip esp32s2 write_flash 0x0 espkvm-dongle-web.bin
```

## First boot checklist

1. Hub on, OLED shows "No dongles paired".
2. Dongle in a PC, LED blinking slowly (unpaired).
3. Hold the encoder button 3 s → "PAIRING MODE".
4. Press BOOT on the dongle → hub shows "Paired PC 0". Done — type away.
5. Repeat for each dongle; switch with the knob or double-tap Right-Ctrl
   then a digit.

See [PAIRING.md](PAIRING.md) for the full user guide and
[SECURITY.md](SECURITY.md) for what the radio is actually doing.
