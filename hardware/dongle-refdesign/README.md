# espkvm stick dongle — custom PCB reference design

A thumb-drive-style espkvm dongle that plugs straight into a USB-A port:
no cable, no dev board. This document is a complete build spec — schematic
netlist, BOM, layout rules — ready to be drawn in KiCad and fabbed at
JLCPCB/PCBWay with assembly.

**Reality check first:** an assembled unit lands around **$6–9/pc at qty 10**
(PCB + parts + assembly + shipping), vs ~$4.50 for an S2 Mini with a USB-A
adapter, and ~$13 for an off-the-shelf cased M5Stack AtomS3U. Build this for
the form factor and the fun, not to save money.

## Concept

```
 ┌──────────────┬───────────────────────────────┬─────────────┐
 │ USB-A plug   │  LDO + passives   BOOT  LED   │ ESP32-S2-   │
 │ (SMD, male)  │                    (o)  (*)   │ MINI-2 ──▶  │  ← antenna
 └──────────────┴───────────────────────────────┴─────────────┘   overhang
   ~15 mm wide, ~45 mm long, 1.6 mm thick, optional printed shell
```

- Runs the stock `firmware/dongle` image, **zero code changes** (LED on
  GPIO15, BOOT on GPIO0 — same defaults as the S2 Mini).
- First flash happens **through the USB-A plug itself**: the ESP32-S2 ROM
  exposes USB-DFU/CDC download mode on the same D+/D− pins — hold BOOT
  while plugging in, then `idf.py dfu-flash` (or esptool over the CDC
  port). No UART header needed.

## BOM (qty 1)

| Ref | Part | Package | Notes |
|-----|------|---------|-------|
| U1 | **ESP32-S2-MINI-2-N4** module | SMD module | FCC/CE-certified, PCB antenna, 4 MB flash. S3 option below. |
| U2 | **AP2112K-3.3** LDO, 600 mA | SOT-25 | Wi-Fi TX peaks ~350 mA — don't go smaller (no AMS1117: dropout too high from 5 V is fine, but idle burn isn't; AP2112K is the standard choice) |
| J1 | USB-A male plug, SMD, horizontal | e.g. "USB AM 90° SMT plug" | On 1.6 mm board. (Alternative: PCB-edge fingers on a 2.0 mm board — cheaper but slightly loose in some ports) |
| D1 | USBLC6-2SC6 ESD array | SOT-666 | On D+/D− (optional but cheap insurance) |
| SW1 | Tactile switch 3×4 mm SMD | — | BOOT / pairing button, reachable when plugged in |
| LED1 | 0603 LED, any color | 0603 | status LED |
| R1 | 1 kΩ | 0603 | LED series |
| R2 | 10 kΩ | 0603 | EN pull-up |
| C1 | 10 µF | 0805 | 5 V in |
| C2, C4 | 100 nF | 0603 | LDO out + module decoupling |
| C3 | 22 µF | 0805 | 3V3 near module |
| C5 | 1 µF | 0603 | EN delay (with R2 = clean power-on reset) |

## Netlist

```
J1.VBUS ── C1 ── U2.VIN
J1.GND  ── GND (all grounds common; J1 shield → GND)
J1.D-   ── D1 ── U1.GPIO19
J1.D+   ── D1 ── U1.GPIO20
U2.VOUT ── C2, C3 ── U1.3V3 (module pin 3V3) ── C4
U2.EN   ── U2.VIN
U1.EN   ── R2 ── 3V3,  U1.EN ── C5 ── GND
U1.GPIO0 ── SW1 ── GND          (internal pull-up used by firmware)
U1.GPIO15 ── R1 ── LED1 ── GND  (firmware drives it high = on)
```

Direct D+/D− connection to the module (no series resistors) is per
Espressif's own S2/S3 reference designs — the PHY is internal.

## Layout rules (the ones that actually matter)

1. **Antenna keepout:** the module's meander antenna must overhang the PCB
   edge (opposite end from the USB plug) or sit over a copper-free zone —
   no copper, any layer, under or within ~5 mm of it. This is the #1 way
   these builds fail.
2. Keep D+/D− as a matched pair, < 30 mm, over solid ground — at
   full-speed USB this is forgiving, don't overthink beyond that.
3. C3 within 3 mm of the module's 3V3 pin; LDO input cap next to J1.
4. BOOT button on the top face near the free end, so it's pressable while
   the dongle is inserted (that's the pairing gesture!).
5. Board width ≤ 16 mm clears the neighbouring USB ports on most hubs.
6. Panelize 10-up for assembly pricing.

## ESP32-S3 variant

Swap U1 for **ESP32-S3-MINI-1-N8**; the pinout of D+/D− (GPIO19/20), EN,
GPIO0 and 3V3 is compatible with this circuit. Build the firmware with
`idf.py set-target esp32s3` (CI already produces a `dongle-s3` image).

## Off-the-shelf alternatives (no PCB required)

| Option | Price | Firmware |
|--------|------:|----------|
| S2 Mini + USB-C-female→USB-A-male adapter | ~$4.50 | stock `esp32s2` image |
| **M5Stack AtomS3U** — USB-A plug, cased, button on GPIO41 | ~$13 | `esp32s3` image, set `ESPKVM_BTN_GPIO=41` (default for S3), LED off |
| LILYGO T-Dongle-S3 — USB-A plug, tiny LCD, BOOT on GPIO0 | ~$12 | `esp32s3` image, set `ESPKVM_BTN_GPIO=0` |
