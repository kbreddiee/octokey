# espkvm hub — complete schematic (v1.0)

Every net, pin, and value needed to capture this in KiCad/EasyEDA and lay
out the board. Module and IC pins are referenced by **pin name** (what EDA
symbols use); verify tiny-package pin *numbers* against the datasheet
footprint as you place them — that step is yours by design.

Firmware pin map is locked to this schematic (`ui_headless.c`,
`Kconfig.projbuild`): **LEDs GPIO4–13, button GPIO14, USB GPIO19/20,
battery sense GPIO1, charge sense GPIO2, UART GPIO43/44.**

---

## Part list (BOM)

JLCPCB/LCSC stocks every line — search the LCSC parts library by the MPN
given. Prefer "Basic" parts where offered (no feeder fee).

| Ref | MPN | Manufacturer | Package | Function |
|-----|-----|--------------|---------|----------|
| U1 | ESP32-S3-WROOM-1-N8 | Espressif | SMD module | MCU + antenna |
| U2 | TP4056 (TP4056-42-ESOP8) | TopPower/NanJing | ESOP-8 | Li-ion charger, 4.2 V |
| U3 | DW01A | Fortune | SOT-23-6 | cell protection controller |
| U4 | FS8205A | Fortune | TSSOP-8 | dual N-MOSFET for U3 |
| U5 | MT3608 | XI/Aerosemi | SOT-23-6 | 5 V boost converter |
| U6 | AP2112K-3.3TRG1 | Diodes Inc. | SOT-25 | 3.3 V LDO, 600 mA |
| U7 | CH340C | WCH | SOP-16 | USB-UART (no crystal needed) |
| U8 | USBLC6-2SC6 | ST | SOT-666 | USB ESD protection |
| Q1, Q2 | S8050 | any | SOT-23 | auto-download transistors |
| D1 | SS34 | any | SMA | boost rectifier, 3 A Schottky |
| L1 | 22 µH power inductor, ≥2 A Isat, e.g. SWPA6045S220MT | Sunlord | 6045 | boost inductor |
| F1 | polyfuse 500 mA hold, e.g. mSMD050 | — | 1206 | USB-A VBUS |
| J1 | USB-C receptacle 16-pin, e.g. TYPE-C-31-M-12 | Korean HRoparts | SMD | charge + flash |
| J2 | USB-A receptacle, through-hole | any | THT | keyboard / dongle port |
| BT1 | 18650 holder, THT (e.g. BH-18650-A1AJ005) | MYOUNG | THT | battery slot |
| SW1 | 6×6 THT tactile | any | THT | MODE |
| SW2, SW3 | 3×4 SMD tactile | any | SMD | BOOT, RESET |
| LED0–9 | 0805 LED (pick your color) | any | 0805 | slot LEDs |
| LED10 | 0603 LED red | any | 0603 | charging |
| LED11 | 0603 LED green | any | 0603 | 3V3 power |

Passives (0603 unless noted): see values inline below.

---

## Block 1 — USB-C input: charging + hub flashing

```
J1 (USB-C receptacle)
  VBUS (A4,B4,A9,B9) ──●── VBUS_IN ──── U2.VCC
  GND  (A1,B1,A12,B12,shell) ── GND
  CC1 (A5) ──[R1 5.1k]── GND        (both CCs: makes any C-C or A-C
  CC2 (B5) ──[R2 5.1k]── GND         cable/charger supply 5 V)
  D+  (A6+B6 tied) ──── U7.UD+
  D−  (A7+B7 tied) ──── U7.UD−
```

## Block 2 — charger (TP4056) + charge status

```
VBUS_IN ──── U2.VCC ── C1 10µF ── GND
U2.CE   ── VBUS_IN            (always enabled when USB present)
U2.TEMP ── GND                (thermistor sensing disabled)
U2.PROG ──[R3 2.0k]── GND     (=> 500 mA charge current)
U2.GND  ── GND
U2.BAT  ──●── BAT ── C2 10µF ── GND
U2./CHRG ──●──[R5 2.0k]──[LED10 ▶]── VBUS_IN   (red = charging)
           └── U1.GPIO2  with [R6 100k] pull-up to 3V3
U2./STDBY ── n.c.
```

## Block 3 — 18650 slot + protection (DW01A + FS8205A)

Copy of the reference circuit on every TP4056 protection module; the
MOSFET pair sits in the cell's negative path.

```
BT1.+  ──●── BAT   (to U2.BAT and U5 input)
BT1.−  ──●── CELL_N
DW01A: VCC ──[R7 470R]── BAT ,  C3 100nF VCC→CELL_N
       GND ── CELL_N
       OD  ── U4 gate A        (discharge FET)
       OC  ── U4 gate B        (charge FET)
       CS  ──[R8 1.0k]── GND   (senses pack ground)
FS8205A: both sources/drains arranged per datasheet typical circuit:
       CELL_N ──[FET A]──[FET B]── GND (pack ground)
```

## Block 4 — 5 V boost (always-on) + 3.3 V rail

```
BAT ──●── L1 22µH ──●── U5.SW
      └── U5.VIN (+C4 10µF)     U5.EN ── U5.VIN   (always on — never
U5.SW ──[D1 SS34 ▶]──●── 5V     use an auto-off powerbank IC here)
U5.FB ──[R9 110k]── 5V
U5.FB ──[R10 15k]── GND         (Vout = 0.6 V × (1+110/15) ≈ 5.0 V)
5V ── C5 22µF ── GND

5V ── U6.VIN (+C6 10µF)   U6.EN ── U6.VIN
U6.VOUT ── 3V3 ── C7 10µF + C8 100nF ── GND
3V3 ──[R11 2.0k]──[LED11 ▶]── GND       (green = power)
```

Power budget: ESP32-S3 Wi-Fi ≈100 mA avg (bursts 350 mA) + receiver
≤50 mA + LED ≈5 mA → ~0.7 W. 2600 mAh cell ≈ 12 h.

## Block 5 — ESP32-S3 module core

```
U1.3V3 ── 3V3 (+C9 10µF + C10 100nF close to pin)
U1.GND ── GND
U1.EN  ──[R12 10k]── 3V3 ,  C11 1µF ── GND ,  SW3 (RESET) ── GND
U1.GPIO0 ── SW2 (BOOT) ── GND     (also driven by Q2, block 7)
Strapping: GPIO45, GPIO46, GPIO3 — leave unconnected.
GPIO35/36/37 — leave unconnected (used internally on octal-PSRAM variants).
```

## Block 6 — USB-A keyboard / dongle-provisioning port

```
5V ──[F1 polyfuse 500mA]── J2.VBUS
J2.D− ── U8.IO1 ── U1.GPIO19
J2.D+ ── U8.IO2 ── U1.GPIO20
J2.GND ── GND
U8.VBUS ── 5V ,  U8.GND ── GND
```

Keep D+/D− as a matched pair over ground, < 30 mm.

## Block 7 — CH340C UART bridge + auto-download

```
U7.VCC ── 3V3 ,  U7.V3 ── 3V3 (tie together for 3.3 V operation)
U7.GND ── GND  (+C12 100nF at VCC)
U7.UD+ / U7.UD− ── J1 data pins (block 1)
U7.TXD ── U1.GPIO44 (RXD)
U7.RXD ── U1.GPIO43 (TXD)

Auto-download (standard ESP devkit cross-pair):
U7.DTR# ──[R13 12k]── Q1.B   Q1.C ── U1.GPIO0   Q1.E ── U7.RTS#
U7.RTS# ──[R14 12k]── Q2.B   Q2.C ── U1.EN      Q2.E ── U7.DTR#
```

Result: `idf.py flash monitor` over USB-C, no buttons.

## Block 8 — slot LEDs + MODE button + battery sense

```
U1.GPIO4..GPIO13 ──[R 1.0k ×10]──[LED0..LED9 ▶]── GND
    (row order = slot order 0..9; silkscreen digits underneath)
U1.GPIO14 ── SW1 (MODE) ── GND      (internal pull-up in firmware)
BAT ──[R15 100k]──●── U1.GPIO1 ──[R16 100k]── GND , C13 100nF ── GND
    (VBAT/2 into ADC — battery gauge, future firmware)
```

---

## Layout reminders (the ones that ruin boards)

1. **Module antenna** overhangs the board edge; no copper any layer under
   it; keep the metal 18650 cell ≥10 mm away.
2. Boost loop (L1/D1/C5/U5) tight; keep its SW node away from GPIO1's
   divider and USB D±.
3. USB-C CC resistors close to J1; without them C-C cables won't power it.
4. TP4056 pad + FS8205 need copper pour for heat at 500 mA charge.
5. Test points: 5V, 3V3, BAT, GND, GPIO43/44.

## Bring-up checklist (before soldering the module)

1. Populate power blocks only → USB-C in: BAT charges (LED10 on),
   5V rail = 5.0±0.2 V, 3V3 = 3.3 V, on battery alone too.
2. Then module + rest → USB-C to PC: CH340 enumerates, `esptool.py
   chip_id` works hands-free (auto-download).
3. Flash `espkvm-hub-headless-web.bin` + dongle pack at 0x290000. LED0
   heartbeat = alive.
