#!/usr/bin/env python3
"""Generate the espkvm hub schematic as a monochrome SVG.

Pure-stdlib drawing helpers; regenerate with:  python gen_schematic.py
Output: schematic.svg (black on white, one A2-ish sheet, 8 framed blocks).
"""

E = []          # svg elements
W, H = 1720, 2400

# ---------------------------------------------------------------- helpers

def add(s): E.append(s)

def line(x1, y1, x2, y2, w=1.6):
    add(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" '
        f'stroke="black" stroke-width="{w}"/>')

def wire(*pts):
    d = "M" + " L".join(f"{x},{y}" for x, y in pts)
    add(f'<path d="{d}" fill="none" stroke="black" stroke-width="1.6"/>')

def dot(x, y):
    add(f'<circle cx="{x}" cy="{y}" r="3.4" fill="black"/>')

def text(x, y, s, size=12, anchor="start", bold=False, mono=True):
    fam = "Consolas,monospace" if mono else "Arial"
    wgt = ' font-weight="bold"' if bold else ""
    s = s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    add(f'<text x="{x}" y="{y}" font-family="{fam}" font-size="{size}"'
        f'{wgt} text-anchor="{anchor}" fill="black">{s}</text>')

def frame(x, y, w, h, title):
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="none" '
        f'stroke="black" stroke-width="2.4"/>')
    text(x + 10, y + 22, title, 15, bold=True)

def flag(x, y, name, side="e"):
    """Net label flag. side e: text to the right; w: to the left."""
    if side == "e":
        wire((x, y), (x + 14, y))
        add(f'<rect x="{x+14}" y="{y-10}" width="{9*len(name)+10}" height="20" '
            f'fill="white" stroke="black" stroke-width="1.2"/>')
        text(x + 19, y + 4, name, 12, bold=True)
    else:
        wire((x - 14, y), (x, y))
        wdt = 9 * len(name) + 10
        add(f'<rect x="{x-14-wdt}" y="{y-10}" width="{wdt}" height="20" '
            f'fill="white" stroke="black" stroke-width="1.2"/>')
        text(x - 19, y + 4, name, 12, anchor="end", bold=True)

def gnd(x, y):
    wire((x, y), (x, y + 12))
    line(x - 11, y + 12, x + 11, y + 12, 2)
    line(x - 7, y + 17, x + 7, y + 17, 2)
    line(x - 3, y + 22, x + 3, y + 22, 2)

def rail(x, y, name):
    """Power rail arrow-ish flag pointing up."""
    wire((x, y), (x, y - 12))
    line(x - 9, y - 12, x + 9, y - 12, 2.2)
    text(x, y - 18, name, 12, anchor="middle", bold=True)

def res_h(x, y, ref, val, l=56):
    """Horizontal resistor centered box; returns (x_left, x_right)."""
    add(f'<rect x="{x}" y="{y-8}" width="{l}" height="16" fill="white" '
        f'stroke="black" stroke-width="1.6"/>')
    text(x + l / 2, y - 13, f"{ref} {val}", 11, anchor="middle")
    return x, x + l

def res_v(x, y, ref, val, l=52):
    add(f'<rect x="{x-8}" y="{y}" width="16" height="{l}" fill="white" '
        f'stroke="black" stroke-width="1.6"/>')
    text(x + 13, y + l / 2 + 4, f"{ref} {val}", 11)
    return y, y + l

def cap_v(x, y, ref, val):
    """Vertical capacitor, plates at y+8..y+16; stub len 8 each side."""
    wire((x, y), (x, y + 8))
    line(x - 11, y + 8, x + 11, y + 8, 2.4)
    line(x - 11, y + 16, x + 11, y + 16, 2.4)
    wire((x, y + 16), (x, y + 24))
    text(x + 15, y + 16, f"{ref} {val}", 11)
    return y, y + 24

def diode_h(x, y, ref, val, led=False, rev=False):
    """Anode left at x, cathode right at x+36 (rev: cathode left)."""
    wire((x, y), (x + 8, y))
    if rev:
        add(f'<path d="M{x+28},{y-9} L{x+28},{y+9} L{x+10},{y} Z" fill="white" '
            f'stroke="black" stroke-width="1.6"/>')
        line(x + 10, y - 9, x + 10, y + 9, 2.2)
    else:
        add(f'<path d="M{x+8},{y-9} L{x+8},{y+9} L{x+26},{y} Z" fill="white" '
            f'stroke="black" stroke-width="1.6"/>')
        line(x + 26, y - 9, x + 26, y + 9, 2.2)
    wire((x + 28 if rev else x + 26, y), (x + 36, y))
    lbl = f"{ref} {val}"
    text(x + 18, y - 14, lbl, 11, anchor="middle")
    if led:
        line(x + 14, y - 16, x + 20, y - 22, 1.2)
        line(x + 20, y - 22, x + 17, y - 21, 1.2)
    return x, x + 36

def inductor_h(x, y, ref, val):
    p = f"M{x},{y}"
    for i in range(4):
        p += f" A7,7 0 0 1 {x+14*(i+1)},{y}"
    add(f'<path d="{p}" fill="none" stroke="black" stroke-width="1.8"/>')
    text(x + 28, y - 12, f"{ref} {val}", 11, anchor="middle")
    return x, x + 56

def npn(x, y, ref):
    """NPN: base stub left at (x-26,y); collector top (x,y-30);
    emitter bottom (x,y+30)."""
    add(f'<circle cx="{x}" cy="{y}" r="17" fill="white" stroke="black" '
        f'stroke-width="1.6"/>')
    line(x - 8, y - 11, x - 8, y + 11, 2.4)          # base bar
    wire((x - 26, y), (x - 8, y))
    line(x - 8, y - 5, x + 5, y - 13, 1.6)           # collector leg
    wire((x + 5, y - 13), (x + 5, y - 17), (x, y - 17), (x, y - 30))
    line(x - 8, y + 5, x + 5, y + 13, 1.6)           # emitter leg
    add(f'<path d="M{x+5},{y+13} l-8,-1 l4,7 z" fill="black"/>')
    wire((x + 5, y + 13), (x + 5, y + 17), (x, y + 17), (x, y + 30))
    text(x + 20, y + 4, ref, 12, bold=True)
    return (x - 26, y), (x, y - 30), (x, y + 30)     # B, C, E

def button(x, y, ref):
    """Two terminals: left (x,y) right (x+44,y)."""
    wire((x, y), (x + 10, y))
    dot(x + 10, y); dot(x + 34, y)
    line(x + 8, y - 10, x + 36, y - 10, 1.8)
    line(x + 22, y - 10, x + 22, y - 16, 1.8)
    wire((x + 34, y), (x + 44, y))
    text(x + 22, y - 20, ref, 11, anchor="middle")
    return (x, y), (x + 44, y)

PIN_L = 22   # pin stub length

def ic(x, y, w, ref, mpn, left, right, pitch=26):
    """IC box. left/right: list of (pin_no or '', name). Returns dict
    name->(x,y) of stub ends."""
    n = max(len(left), len(right))
    h = pitch * (n + 1)
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="white" '
        f'stroke="black" stroke-width="2.2"/>')
    text(x + w / 2, y - 22, ref, 14, anchor="middle", bold=True)
    text(x + w / 2, y - 6, mpn, 12, anchor="middle")
    pins = {}
    for i, (num, name) in enumerate(left):
        py = y + pitch * (i + 1)
        line(x - PIN_L, py, x, py, 1.8)
        text(x + 6, py + 4, name, 12)
        if num != "":
            text(x - 6, py - 5, str(num), 10, anchor="end")
        pins[name] = (x - PIN_L, py)
    for i, (num, name) in enumerate(right):
        py = y + pitch * (i + 1)
        line(x + w, py, x + w + PIN_L, py, 1.8)
        text(x + w - 6, py + 4, name, 12, anchor="end")
        if num != "":
            text(x + w + 6, py - 5, str(num), 10)
        pins[name] = (x + w + PIN_L, py)
    return pins, h

# ================================================================ SHEET

add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
    f'viewBox="0 0 {W} {H}">')
add(f'<rect width="{W}" height="{H}" fill="white"/>')
text(W / 2, 34, "espkvm hub — schematic v1.0 (custom PCB, headless: 10 slot LEDs + MODE button)",
     20, anchor="middle", bold=True)
text(W / 2, 56, "monochrome sheet generated by gen_schematic.py — pin numbers: verify against datasheet footprints before ordering",
     12, anchor="middle")

# ---------------------------------------------------------------- B1: USB-C + CH340C + auto-download
frame(30, 80, 830, 640, "1  USB-C: charge input + hub flashing (CH340C + auto-download)")

# USB-C connector as IC-style box
jp, jh = ic(70, 130, 130, "J1", "USB-C 16p",
            [], [("A4/B4/A9/B9", "VBUS"), ("A5", "CC1"), ("B5", "CC2"),
                 ("A6/B6", "D+"), ("A7/B7", "D-"),
                 ("A1/B1/A12/B12", "GND"), ("", "SHELL")], pitch=30)

# VBUS -> net flag
vx, vy = jp["VBUS"]
wire((vx, vy), (vx + 240, vy))
flag(vx + 240, vy, "VBUS_IN")
# CC resistors
for i, cc in enumerate(["CC1", "CC2"]):
    cx, cy = jp[cc]
    x0, x1 = res_h(cx + 40, cy, f"R{i+1}", "5.1k")
    wire((cx, cy), (x0, cy))
    wire((x1, cy), (x1 + 12, cy))
    gnd(x1 + 12, cy)
# GND + shell
gx, gy = jp["GND"]
wire((gx, gy), (gx + 18, gy)); gnd(gx + 18, gy)
sx, sy = jp["SHELL"]
wire((sx, sy), (sx + 18, sy)); gnd(sx + 18, sy)

# CH340C
cp, ch = ic(560, 130, 150, "U7", "CH340C SOP-16",
            [(5, "UD+"), (6, "UD-"), (16, "VCC"), (4, "V3"), (1, "GND")],
            [(2, "TXD"), (3, "RXD"), (13, "DTR#"), (14, "RTS#"), ("", "NC*")])
# usb data wires
dx, dy = jp["D+"]; ux, uy = cp["UD+"]
wire((dx, dy), (dx + 14, dy), (dx + 14, uy), (ux, uy))
dx, dy = jp["D-"]; ux, uy = cp["UD-"]
wire((dx, dy), (dx + 26, dy), (dx + 26, uy), (ux, uy))
# CH340 power: VCC+V3 -> 3V3
vcx, vcy = cp["VCC"]; v3x, v3y = cp["V3"]
wire((vcx, vcy), (vcx - 24, vcy), (vcx - 24, v3y), (v3x, v3y))
dot(vcx - 24, vcy)
rail(vcx - 24, vcy, "3V3")
cy0, cy1 = cap_v(vcx - 60, vcy + 6, "C12", "100n")
wire((vcx - 24, vcy), (vcx - 60, vcy), (vcx - 60, cy0))
gnd(vcx - 60, cy1)
cgx, cgy = cp["GND"]
wire((cgx, cgy), (cgx - 18, cgy)); gnd(cgx - 18, cgy)
# TXD/RXD flags
tx, ty = cp["TXD"]; wire((tx, ty), (tx + 30, ty)); flag(tx + 30, ty, "U1.RXD/IO44")
rx, ry = cp["RXD"]; wire((rx, ry), (rx + 30, ry)); flag(rx + 30, ry, "U1.TXD/IO43")
text(635, 130 + ch + 18, "* pins 7,8,9,10,11,12,15 n.c.", 10, anchor="middle")

# auto-download transistors
dtx, dty = cp["DTR#"]; rtx, rty = cp["RTS#"]
q1B, q1C, q1E = npn(430, 560, "Q1 S8050")
q2B, q2C, q2E = npn(660, 560, "Q2 S8050")
# base resistors
x0, x1 = res_h(q1B[0] - 76, q1B[1], "R13", "12k")
wire((x1, q1B[1]), q1B)
x2, x3 = res_h(q2B[0] - 76, q2B[1], "R14", "12k")
wire((x3, q2B[1]), q2B)
# DTR -> R13, and DTR -> Q2.E
wire((dtx, dty), (dtx + 14, dty), (dtx + 14, 480), (300, 480), (300, q1B[1]), (x0, q1B[1]))
wire((300, q1B[1]), (300, 640), (q2E[0], 640), q2E)
dot(300, q1B[1])
# RTS -> R14, and RTS -> Q1.E
wire((rtx, rty), (rtx + 26, rty), (rtx + 26, 505), (520, 505), (520, q2B[1]), (x2, q2B[1]))
wire((520, q2B[1]), (520, 658), (q1E[0], 658), q1E)
dot(520, q2B[1])
# collectors -> flags
wire(q1C, (q1C[0], q1C[1] - 16)); flag(q1C[0], q1C[1] - 16 - 0, "U1.IO0")
wire(q2C, (q2C[0], q2C[1] - 16)); flag(q2C[0], q2C[1] - 16, "U1.EN")
text(56, 700, "auto-download: DTR/RTS cross-pair drives EN+IO0 (idf.py flash, no buttons)", 11)

# ---------------------------------------------------------------- B2: charger
frame(890, 80, 800, 420, "2  Li-ion charger (500 mA) + charge status")
tp, th = ic(1150, 140, 170, "U2", "TP4056 ESOP-8",
            [(4, "VCC"), (8, "CE"), (1, "TEMP"), (2, "PROG"), (3, "GND")],
            [(5, "BAT"), (7, "/CHRG"), (6, "/STDBY")], pitch=30)
# VCC from VBUS_IN
vx, vy = tp["VCC"]
wire((vx, vy), (vx - 100, vy))
flag(vx - 100, vy, "VBUS_IN", side="w")
cy0, cy1 = cap_v(vx - 60, vy + 8, "C1", "10µF")
wire((vx - 60, vy), (vx - 60, cy0)); dot(vx - 60, vy)
gnd(vx - 60, cy1)
# CE tied up to VCC
cex, cey = tp["CE"]
wire((cex, cey), (vx - 30, cey), (vx - 30, vy))
dot(vx - 30, vy)
# TEMP to gnd
tx, ty = tp["TEMP"]
wire((tx, ty), (tx - 20, ty)); gnd(tx - 20, ty)
# PROG resistor
px, py = tp["PROG"]
y0, y1 = res_v(px - 46, py + 10, "R3", "2.0k")
wire((px, py), (px - 46, py), (px - 46, y0))
gnd(px - 46, y1)
# GND
gx, gy = tp["GND"]
wire((gx, gy), (gx - 20, gy), (gx - 20, gy + 34)); gnd(gx - 20, gy + 34)
# BAT out
bx, by = tp["BAT"]
wire((bx, by), (bx + 170, by))
flag(bx + 170, by, "BAT")
cy0, cy1 = cap_v(bx + 60, by + 8, "C2", "10µF")
wire((bx + 60, by), (bx + 60, cy0)); dot(bx + 60, by)
gnd(bx + 60, cy1)
# /CHRG: open-drain node -> LED chain to VBUS_IN, GPIO sense + pull-up
hx, hy = tp["/CHRG"]
n2y = hy + 90
wire((hx, hy), (hx + 20, hy), (hx + 20, n2y), (hx + 40, n2y))
dot(hx + 20, n2y)
d0, d1 = diode_h(hx + 40, n2y, "LED10", "red", led=True, rev=True)
x0, x1 = res_h(d1 + 22, n2y, "R5", "2.0k")
wire((d1, n2y), (x0, n2y))
wire((x1, n2y), (x1 + 20, n2y))
flag(x1 + 20, n2y, "VBUS_IN")
text(hx + 40, n2y + 26, "cathode at /CHRG: lit while charging", 10)
# gpio sense + pull-up, routed left under the charger
n3y = n2y + 80
wire((hx + 20, n2y), (hx + 20, n3y), (980, n3y))
flag(980, n3y, "U1.IO2", side="w")
y0, y1 = res_v(1050, n3y - 66, "R6", "100k")
wire((1050, n3y), (1050, y1)); dot(1050, n3y)
rail(1050, y0, "3V3")
# STDBY nc
sx, sy = tp["/STDBY"]
text(sx + 6, sy + 4, "n.c.", 11)

# ---------------------------------------------------------------- B3: cell + protection
frame(890, 520, 800, 470, "3  18650 slot + protection (datasheet reference circuit)")
bt, bth = ic(950, 580, 120, "BT1", "18650 holder",
             [], [("", "B+"), ("", "B-")], pitch=40)
bpx, bpy = bt["B+"]; bnx, bny = bt["B-"]
wire((bpx, bpy), (bpx + 20, bpy))
dot(bpx + 20, bpy)
flag(bpx + 20, bpy, "BAT")
wire((bnx, bny), (bnx + 66, bny))
dot(bnx + 66, bny)
text(bnx + 10, bny + 24, "CELL_N", 12, bold=True)

dw, dwh = ic(1240, 570, 150, "U3", "DW01A SOT23-6",
             [(5, "VCC"), (6, "GND"), (2, "CS")],
             [(1, "OD"), (3, "OC"), (4, "TD")])
fs, fsh = ic(1240, 800, 150, "U4", "FS8205A TSSOP-8",
             [("", "GATE_A"), ("", "GATE_B"), ("", "CELL_N")],
             [("", "PACK_GND")], pitch=30)
# wiring
vx, vy = dw["VCC"]
x0, x1 = res_h(1130, vy, "R7", "470R")
wire((x1, vy), (vx, vy))
wire((bpx + 20, bpy), (bpx + 20, vy - 24), (1112, vy - 24))
wire((1112, vy - 24), (1112, vy), (x0, vy))
gx, gy = dw["GND"]
wire((gx, gy), (gx - 60, gy), (gx - 60, bny), (bnx + 66, bny))
csx, csy = dw["CS"]
y0, y1 = res_v(csx - 40, csy + 10, "R8", "1.0k")
wire((csx, csy), (csx - 40, csy), (csx - 40, y0))
gnd(csx - 40, y1)
odx, ody = dw["OD"]; ocx, ocy = dw["OC"]
gax, gay = fs["GATE_A"]; gbx, gby = fs["GATE_B"]
wire((odx, ody), (odx + 40, ody), (odx + 40, 760), (gax - 60, 760), (gax - 60, gay), (gax, gay))
wire((ocx, ocy), (ocx + 20, ocy), (ocx + 20, 775), (gbx - 40, 775), (gbx - 40, gby), (gbx, gby))
cnx, cny = fs["CELL_N"]
wire((cnx, cny), (cnx - 30, cny))
text(cnx - 100, cny + 4, "CELL_N", 12, bold=True)
pgx, pgy = fs["PACK_GND"]
wire((pgx, pgy), (pgx + 16, pgy)); gnd(pgx + 16, pgy)
tdx, tdy = dw["TD"]
text(tdx + 6, tdy + 4, "n.c.", 11)
text(910, 970, "C3 100nF across U3.VCC-CELL_N. FETs sit in the cell's negative path.", 11)

# ---------------------------------------------------------------- B4: boost + LDO
frame(30, 740, 830, 480, "4  5V boost (always-on) + 3.3V rail")
mt, mth = ic(300, 800, 150, "U5", "MT3608 SOT23-6",
             [(5, "VIN"), (4, "EN"), (2, "GND")],
             [(1, "SW"), (3, "FB")])
vx, vy = mt["VIN"]
wire((vx, vy), (vx - 90, vy))
flag(vx - 90, vy, "BAT", side="w")
dot(vx - 40, vy)
cy0, cy1 = cap_v(vx - 40, vy + 8, "C4", "10µF")
wire((vx - 40, vy), (vx - 40, cy0)); gnd(vx - 40, cy1)
ex, ey = mt["EN"]
wire((ex, ey), (ex - 20, ey), (ex - 20, vy))
dot(ex - 20, vy)
ggx, ggy = mt["GND"]
wire((ggx, ggy), (ggx - 20, ggy), (ggx - 20, ggy + 26)); gnd(ggx - 20, ggy + 26)
# inductor from VIN node to SW
swx, swy = mt["SW"]
lx0, lx1 = inductor_h(vx - 40 - 0 + 130, 770, "L1", "22µH")
wire((vx - 40, vy), (vx - 40, 770), (lx0, 770))
wire((lx1, 770), (swx + 30, 770), (swx + 30, swy), (swx, swy))
dot(swx + 30, swy)
# diode SW->5V
dx0, dx1 = diode_h(swx + 30, swy + 0, "D1", "SS34")
n5x = dx1 + 90
wire((dx1, swy), (n5x + 40, swy))
dot(n5x, swy)
rail(n5x + 40, swy, "5V")
cy0, cy1 = cap_v(n5x, swy + 8, "C5", "22µF")
wire((n5x, swy), (n5x, cy0)); gnd(n5x, cy1)
# FB divider from 5V node
fbx, fby = mt["FB"]
wire((fbx, fby), (fbx + 40, fby), (fbx + 40, fby + 118))
fnode = (fbx + 40, fby + 118)
x0, x1 = res_h(fnode[0] - 130, fnode[1], "R10", "15k")
wire((x0 - 12, fnode[1]), (x0, fnode[1]))
gnd(x0 - 12, fnode[1])
wire((x1, fnode[1]), fnode)
dot(*fnode)
x0, x1 = res_h(fnode[0] + 10, fnode[1], "R9", "110k")
wire(fnode, (x0, fnode[1]))
wire((x1, fnode[1]), (n5x, fnode[1]), (n5x, swy))
text(70, 1190, "Vout = 0.6V x (1 + 110/15) = 5.0V", 11)

ap, aph = ic(560, 1030, 150, "U6", "AP2112K-3.3 SOT-25",
             [(1, "VIN"), (3, "EN"), (2, "GND")],
             [(5, "VOUT"), (4, "NC")])
vx2, vy2 = ap["VIN"]
wire((vx2, vy2), (vx2 - 60, vy2))
rail(vx2 - 60, vy2, "5V")
dot(vx2 - 34, vy2)
cy0, cy1 = cap_v(vx2 - 34, vy2 + 8, "C6", "10µF")
wire((vx2 - 34, vy2), (vx2 - 34, cy0)); gnd(vx2 - 34, cy1)
ex2, ey2 = ap["EN"]
wire((ex2, ey2), (ex2 - 16, ey2), (ex2 - 16, vy2)); dot(ex2 - 16, vy2)
gx2, gy2 = ap["GND"]
wire((gx2, gy2), (gx2 - 16, gy2), (gx2 - 16, gy2 + 30)); gnd(gx2 - 16, gy2 + 30)
vo_x, vo_y = ap["VOUT"]
wire((vo_x, vo_y), (vo_x + 120, vo_y))
rail(vo_x + 120, vo_y, "3V3")
dot(vo_x + 44, vo_y)
cy0, cy1 = cap_v(vo_x + 44, vo_y + 8, "C7+C8", "10µ,100n")
wire((vo_x + 44, vo_y), (vo_x + 44, cy0)); gnd(vo_x + 44, cy1)
# power LED hangs below the 3V3 run
dot(vo_x + 96, vo_y)
y0, y1 = res_v(vo_x + 96, vo_y + 46, "R11", "2.0k")
wire((vo_x + 96, vo_y), (vo_x + 96, y0))
text(vo_x + 84, y1 + 34, "LED11 green", 11, anchor="end")
add(f'<path d="M{vo_x+96},{y1} l0,6 l-8,0 l8,14 l8,-14 l-8,0" fill="none" stroke="black" stroke-width="1.6"/>')
line(vo_x + 88, y1 + 20, vo_x + 104, y1 + 20, 2)
wire((vo_x + 96, y1 + 20), (vo_x + 96, y1 + 30))
gnd(vo_x + 96, y1 + 30)

# ---------------------------------------------------------------- B5: ESP module
frame(890, 1010, 800, 1000, "5  ESP32-S3-WROOM-1-N8 (U1)")
left_pins = [("", "3V3"), ("", "EN"), ("", "IO0"), ("", "IO1"), ("", "IO2"),
             ("", "IO4"), ("", "IO5"), ("", "IO6"), ("", "IO7"), ("", "IO8"),
             ("", "IO9"), ("", "IO10"), ("", "IO11"), ("", "IO12"),
             ("", "IO13"), ("", "IO14")]
right_pins = [("", "IO19/USB_D-"), ("", "IO20/USB_D+"), ("", "TXD0/IO43"),
              ("", "RXD0/IO44"), ("", "GND"), ("", "IO3  n.c.*"),
              ("", "IO45 n.c.*"), ("", "IO46 n.c.*"), ("", "IO35-37 n.c.")]
u1, u1h = ic(1130, 1080, 300, "U1", "ESP32-S3-WROOM-1-N8", left_pins,
             right_pins, pitch=44)
# 3V3 + decoupling
x, y = u1["3V3"]
wire((x, y), (x - 60, y))
rail(x - 60, y, "3V3")
dot(x - 34, y)
cy0, cy1 = cap_v(x - 34, y + 8, "C9+C10", "10µ+100n")
wire((x - 34, y), (x - 34, cy0)); gnd(x - 34, cy1)
# EN / IO0: net flags only; the RC + buttons live in the detail below
x, y = u1["EN"]
wire((x, y), (x - 30, y))
flag(x - 30, y, "U1.EN", side="w")
x, y = u1["IO0"]
wire((x, y), (x - 30, y))
flag(x - 30, y, "U1.IO0", side="w")
# IO1 battery sense / IO2 chrg
x, y = u1["IO1"]
wire((x, y), (x - 36, y)); flag(x - 36, y, "VBAT/2", side="w")
x, y = u1["IO2"]
wire((x, y), (x - 36, y)); flag(x - 36, y, "U1.IO2", side="w")
# LED gpios
for i in range(10):
    x, y = u1[f"IO{4+i}"]
    wire((x, y), (x - 36, y))
    flag(x - 36, y, f"LED{i}", side="w")
# IO14 button
x, y = u1["IO14"]
wire((x, y), (x - 36, y)); flag(x - 36, y, "MODE", side="w")
# right side
x, y = u1["IO19/USB_D-"]
wire((x, y), (x + 20, y)); flag(x + 20, y, "USB_D-")
x, y = u1["IO20/USB_D+"]
wire((x, y), (x + 20, y)); flag(x + 20, y, "USB_D+")
x, y = u1["TXD0/IO43"]
wire((x, y), (x + 20, y)); flag(x + 20, y, "U1.TXD/IO43")
x, y = u1["RXD0/IO44"]
wire((x, y), (x + 20, y)); flag(x + 20, y, "U1.RXD/IO44")
x, y = u1["GND"]
wire((x, y), (x + 24, y)); gnd(x + 24, y)
text(1240, 1080 + u1h + 28,
     "* strapping/PSRAM pins: leave IO3, IO45, IO46, IO35-37 unconnected", 11)
# reset / boot detail rows
dy1 = 1080 + u1h + 92
flag(1000, dy1, "U1.EN", side="w")
wire((1000, dy1), (1120, dy1))
dot(1050, dy1); dot(1120, dy1)
y0, y1 = res_v(1050, dy1 - 66, "R12", "10k")
wire((1050, dy1), (1050, y1))
rail(1050, y0, "3V3")
cy0, cy1 = cap_v(1120, dy1 + 6, "C11", "1µF")
wire((1120, dy1), (1120, cy0))
gnd(1120, cy1)
b0, b1 = button(1180, dy1, "SW3 RESET")
wire((1120, dy1), (b0[0], dy1))
wire(b1, (b1[0] + 12, dy1)); gnd(b1[0] + 12, dy1)
dy2 = dy1 + 58
flag(1000, dy2, "U1.IO0", side="w")
b0, b1 = button(1180, dy2, "SW2 BOOT")
wire((1000, dy2), (b0[0], dy2))
wire(b1, (b1[0] + 12, dy2)); gnd(b1[0] + 12, dy2)

# ---------------------------------------------------------------- B6: USB-A
frame(30, 1240, 830, 400, "6  USB-A: keyboard / dongle-provisioning port")
ja, jah = ic(90, 1300, 130, "J2", "USB-A THT",
             [], [("1", "VBUS"), ("2", "D-"), ("3", "D+"), ("4", "GND")],
             pitch=34)
es, esh = ic(520, 1300, 160, "U8", "USBLC6-2SC6",
             [("", "IO1"), ("", "IO2"), ("", "GND")],
             [("", "IO1'"), ("", "IO2'"), ("", "VBUS")], pitch=34)
# VBUS via polyfuse from 5V
vx, vy = ja["VBUS"]
x0, x1 = res_h(vx + 50, vy, "F1", "500mA PTC")
wire((vx, vy), (x0, vy))
wire((x1, vy), (x1 + 30, vy))
rail(x1 + 30, vy, "5V")
# data through ESD
dnx, dny = ja["D-"]; i1x, i1y = es["IO1"]
wire((dnx, dny), (dnx + 120, dny), (dnx + 120, i1y), (i1x, i1y))
dpx, dpy = ja["D+"]; i2x, i2y = es["IO2"]
wire((dpx, dpy), (dpx + 100, dpy), (dpx + 100, i2y), (i2x, i2y))
gx, gy = ja["GND"]
wire((gx, gy), (gx + 20, gy)); gnd(gx + 20, gy)
o1x, o1y = es["IO1'"]
wire((o1x, o1y), (o1x + 20, o1y)); flag(o1x + 20, o1y, "USB_D-")
o2x, o2y = es["IO2'"]
wire((o2x, o2y), (o2x + 20, o2y)); flag(o2x + 20, o2y, "USB_D+")
evx, evy = es["VBUS"]
wire((evx, evy), (evx + 30, evy))
rail(evx + 30, evy, "5V")
egx, egy = es["GND"]
wire((egx, egy), (egx - 20, egy), (egx - 20, egy + 26)); gnd(egx - 20, egy + 26)
text(60, 1600, "keep D+/D- matched pair over ground, < 30mm; same port also flashes", 11)
text(60, 1616, "dongles (T-Dongle-S3 with BOOT held) and the hub itself (ROM USB mode)", 11)

# ---------------------------------------------------------------- B7: LEDs + button + divider
frame(30, 1660, 830, 700, "7  slot LEDs, MODE button, battery sense")
for i in range(10):
    y = 1716 + i * 46
    flag(70 + 14, y, f"LED{i}", side="w")   # flag drawn with stub to left
    x0, x1 = res_h(120, y, f"R2{i}", "1.0k")
    wire((84, y), (x0, y))
    d0, d1 = diode_h(x1 + 6, y, f"LED{i}", "", led=True)
    wire((x1, y), (d0, y))
    wire((d1, y), (d1 + 12, y))
    gnd(d1 + 12, y)
text(360, 1700, "one per slot, silkscreen digits 0-9 under the row", 11)
# MODE button
by = 1716 + 10 * 46 + 26
flag(84, by, "MODE", side="w")
b0, b1 = button(120, by, "SW1 MODE")
wire((84, by), (b0[0], by))
wire(b1, (b1[0] + 12, by)); gnd(b1[0] + 12, by)
text(240, by + 4, "(internal pull-up in firmware)", 11)
# battery divider
dy = by + 62
flag(120, dy, "BAT", side="w")
x0, x1 = res_h(140, dy, "R15", "100k")
wire((120, dy), (x0, dy))
nx = x1 + 40
wire((x1, dy), (nx, dy))
dot(nx, dy)
x2, x3 = res_h(nx + 20, dy, "R16", "100k")
wire((nx, dy), (x2, dy))
wire((x3, dy), (x3 + 12, dy)); gnd(x3 + 12, dy)
wire((nx, dy), (nx, dy + 40), (nx + 90, dy + 40))
flag(nx + 90, dy + 40, "VBAT/2")
cy0, cy1 = cap_v(nx + 40, dy + 40 + 6, "C13", "100n")
wire((nx + 40, dy + 40), (nx + 40, cy0)); dot(nx + 40, dy + 40)
gnd(nx + 40, cy1)

# ---------------------------------------------------------------- title block
add(f'<rect x="890" y="2020" width="800" height="340" fill="none" '
    f'stroke="black" stroke-width="2.4"/>')
text(910, 2052, "NOTES", 15, bold=True)
notes = [
    "1. Net flags with the same name are connected (VBUS_IN, BAT, CELL_N,",
    "   5V, 3V3, USB_D+, USB_D-, U1.EN, U1.IO0, U1.IO2, U1.TXD/RXD,",
    "   VBAT/2, MODE, LED0-9).",
    "2. Firmware pin map is locked: LEDs IO4-13, MODE IO14, USB 19/20,",
    "   VBAT sense IO1, /CHRG sense IO2, UART0 43/44 (ui_headless.c).",
    "3. Layout: module antenna overhangs board edge, no copper below,",
    "   18650 cell >= 10mm away. Boost loop (L1/D1/C5) tight, SW node",
    "   away from USB pair and the IO1 divider. CC resistors at J1.",
    "4. Charge current 500mA (R3 2.0k). Never substitute an IP5306-style",
    "   powerbank IC: auto-off at light load kills an idle hub.",
    "5. Pin numbers here are from datasheets believed current - verify",
    "   each footprint against its datasheet before ordering assembly.",
    "6. Repo: github espkvm - hardware/hub-refdesign/ (SCHEMATIC.md has",
    "   the same netlist in text form; firmware: espkvm-hub-headless).",
]
for i, n in enumerate(notes):
    text(910, 2078 + i * 19, n, 11.5)
text(910, 2344, "espkvm hub v1.0 - 2026-07-02 - MIT licensed", 12, bold=True)

add("</svg>")

with open("schematic.svg", "w", encoding="utf-8") as f:
    f.write("\n".join(E))
print(f"wrote schematic.svg ({len(E)} elements)")
