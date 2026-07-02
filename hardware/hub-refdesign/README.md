# espkvm hub — custom PCB reference design

A purpose-built hub board: ESP32-S3, USB-A port for the keyboard,
**rechargeable 18650 battery with USB-C charging**, **10 slot LEDs and one
button** — no display. Runs the stock `firmware/hub` image built with the
`sdkconfig.headless` overlay, zero code changes.

```
 ┌────────────────────────────────────────────────────────┐
 │  USB-C     ○ ○ ○ ○ ○ ○ ○ ○ ○ ○      [MODE]             │
 │ (charge)   0 1 2 3 4 5 6 7 8 9      button   ┌───────┐ │
 │  ⚡chg LED                                    │ USB-A │ │
 │ ┌──────────────────────────────┐             │ (kbd) │ │
 │ │      18650 battery holder    │             └───────┘ │
 │ └──────────────────────────────┘   ESP32-S3-WROOM-1 ─▶ │ ← antenna edge
 └────────────────────────────────────────────────────────┘
   ~95 x 40 mm, 1.6 mm. LED row on top, USB-A on the side.
```

## User interface (matches `ui_headless.c`)

| Gesture | Action | LED feedback |
|---|---|---|
| short press | switch to next paired slot | new slot LED solid |
| hold 3 s | pairing mode (30 s) | chase animation |
| triple-click, then one click | forget active slot | all-LED warning flash first |
| — | active dongle offline | active LED blinks 2 Hz |
| — | hotkey command mode | active LED blinks 8 Hz |

Plus the double-Right-Ctrl hotkey from the keyboard, which needs no hub UI
at all — the dongles' screens show the slot numbers.

## Power architecture

```
USB-C (5V in) ──▶ TP4056 charger ──▶ 18650 cell (holder)
                        │                  │ DW01A+FS8205 protection
                        ▼                  ▼
                 CHRG status LED    TPS61023 boost ──▶ 5V rail
                                           │
                              ┌────────────┼──────────────┐
                              ▼            ▼              ▼
                        USB-A VBUS   AP2112K-3.3 ──▶  ESP32-S3
                        (keyboard,   (3V3 rail)
                         polyfuse)
```

- **Budget:** ESP32-S3 Wi-Fi ~100 mA avg (PS off) + keyboard receiver
  ~30–50 mA + LED ~5 mA ≈ 0.7 W. A 2600 mAh 18650 runs **~12 h** per
  charge; charge and run simultaneously is fine (TP4056 powers the load).
- **Do not use an IP5306-style power-bank IC**: they auto-shut-off below
  ~50 mA load and will kill the hub when idle. The discrete
  TP4056 + protection + always-on boost chain has no such behaviour.
- TP4056 PROG resistor 2 kΩ → 0.5 A charge (gentle on any cell).

## BOM (qty 1)

| Ref | Part | Package | Notes |
|-----|------|---------|-------|
| U1 | ESP32-S3-WROOM-1-N8 | SMD module | certified module, PCB antenna |
| U2 | TP4056 | SOP-8 | Li-ion charger, 0.5 A (R_PROG 2 kΩ) |
| U3 | DW01A + U4 FS8205 | SOT-23-6/TSSOP-8 | cell protection (skip if you mandate protected cells — don't) |
| U5 | TPS61023 | SOT-563 | 5 V boost, 1 A+ from a single cell |
| U6 | AP2112K-3.3 | SOT-25 | 3V3 LDO from the 5 V rail |
| U7 | CH340C | SOP-16 | USB-UART bridge on the USB-C data pins — flash the hub and read logs over USB-C, no buttons needed |
| Q1, Q2 | S8050 (or 2N3904) | SOT-23 | auto-download circuit (DTR/RTS → EN/IO0, the standard NodeMCU two-transistor arrangement) |
| J1 | USB-C receptacle, 16-pin (CC1/CC2 5.1 kΩ to GND) | SMD | charge input **+ hub flashing/console** (data pins → U7) |
| J2 | USB-A receptacle, THT | — | keyboard / receiver port |
| BT1 | 18650 holder, THT | — | the "battery slot" |
| F1 | polyfuse 500 mA | 1206 | on J2 VBUS |
| D1 | USBLC6-2SC6 | SOT-666 | ESD on USB data |
| SW1 | tactile switch 6×6 | THT/SMD | MODE button |
| SW2, SW3 | tactile 3×4 | SMD | BOOT, RESET |
| LED0–9 | 0805 LED + 1 kΩ | — | slot LEDs, one color, in a tidy row |
| LED10 | 0603 LED + 2 kΩ | — | charge indicator from TP4056 /CHRG |
| — | caps: 10 µF ×4, 100 nF ×4, 22 µF ×2; R: 10 kΩ ×3, 100 kΩ ×2, 5.1 kΩ ×2 | 0603/0805 | per IC datasheets |

## Net / pin map (matches firmware defaults exactly)

| ESP32-S3 pin | Net |
|---|---|
| GPIO4..GPIO13 | LED0..LED9 (through 1 kΩ, LED to GND — active high) |
| GPIO14 | MODE button → GND (internal pull-up) |
| GPIO19 / GPIO20 | USB D− / D+ → J2 (USB-A), via D1 |
| GPIO0 | BOOT button → GND |
| EN | RESET button → GND, 10 kΩ to 3V3 + 1 µF to GND |
| GPIO1 | battery voltage: cell → 100 kΩ / 100 kΩ divider (future gauge) |
| GPIO2 | TP4056 /CHRG (open-drain, pull-up) — future "charging" state |
| GPIO43 / GPIO44 | UART0 TX/RX → U7 (CH340C) RXD/TXD |
| EN / GPIO0 | ← Q1/Q2 auto-download circuit from U7 DTR/RTS |

GPIO4–13 are free non-strapping pins on every WROOM-1 variant including
octal-PSRAM N16R8; GPIO35–37 are deliberately unused.

## Flashing

**Over USB-C, hands-free.** The USB-C port's data pins go to a CH340C
USB-UART bridge wired to UART0 with the standard two-transistor
auto-download circuit — so the same cable that charges the hub also does
`idf.py flash monitor` with no button dance, exactly like a devkit. The
S3's native USB stays dedicated to the USB-A keyboard/provisioning port.
(Fallback: hold BOOT, tap RESET, and flash via the USB-A port using the
ROM's USB download mode.)

Firmware build:

```sh
cd firmware/hub
idf.py set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.headless" build
```

CI ships this as `espkvm-hub-headless-web.bin`.

## Layout rules

1. **Antenna overhang** on the edge opposite the 18650 holder — no copper
   under it, and keep the metal battery cell ≥ 10 mm away from it.
2. Boost converter (U5): tight loop, inductor close, keep its switch node
   away from GPIO1's battery divider and from USB D+/D−.
3. USB D+/D− short matched pair over ground to J2.
4. LED row on 2.54 mm-ish pitch with silkscreen digits `0..9` under it.
5. THT 18650 holder and USB-A give the board mechanical rigidity — put
   them on the same face so the back stays flat for rubber feet.

## Cost (JLCPCB assembled, indicative)

| Qty | PCB+assembly+parts | Per unit |
|----:|--------------------:|---------:|
| 5 | ~$70 | ~$14 |
| 10 | ~$95 | ~$9.50 |

Plus one 18650 cell (~$4) per hub. For firmware bring-up while boards are
at the fab, the same headless image runs on a bare ESP32-S3-DevKitC-1 with
LEDs on a breadboard — or just use the DevKit OLED variant for testing.
