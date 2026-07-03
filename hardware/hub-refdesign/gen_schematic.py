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
# One continuous circuit: signals are drawn wires; only GND / 3V3 / 5V
# use power symbols. Crossings without a dot are NOT connected.

W, H = 1720, 1900
add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
    f'viewBox="0 0 {W} {H}">')
add(f'<rect width="{W}" height="{H}" fill="white"/>')
add(f'<rect x="18" y="66" width="{W-36}" height="{H-90}" fill="none" '
    f'stroke="black" stroke-width="2.6"/>')
text(W / 2, 32, "espkvm hub — schematic v1.0 (single sheet, custom PCB: 10 slot LEDs + MODE button)",
     20, anchor="middle", bold=True)
text(W / 2, 54, "wires crossing WITHOUT a dot are not connected — GND / 3V3 / 5V power symbols are the only implicit nets",
     12, anchor="middle")

# ---------------- J1 USB-C (top left) ------------------------------
jp, jh = ic(60, 120, 130, "J1", "USB-C 16p",
            [], [("A4/B4/A9/B9", "VBUS"), ("A5", "CC1"), ("B5", "CC2"),
                 ("A6/B6", "D+"), ("A7/B7", "D-"),
                 ("A1..B12", "GND"), ("", "SHELL")], pitch=30)

# ---------------- U2 TP4056 charger (top middle) --------------------
tp, th = ic(720, 120, 170, "U2", "TP4056 ESOP-8",
            [(4, "VCC"), (8, "CE"), (1, "TEMP"), (2, "PROG"), (3, "GND")],
            [(5, "BAT"), (7, "/CHRG"), (6, "/STDBY")], pitch=30)

vx, vy = jp["VBUS"]
ux, uy = tp["VCC"]
wire((vx, vy), (ux, uy))
text(340, 142, "VBUS_IN", 11, bold=True)
dot(650, vy)
cy0, cy1 = cap_v(650, vy + 8, "C1", "10µF")
gnd(650, cy1)
cex, cey = tp["CE"]
wire((cex, cey), (672, cey), (672, vy)); dot(672, vy)
tx, ty = tp["TEMP"]
wire((tx, ty), (660, ty)); gnd(660, ty)
px, py = tp["PROG"]
wire((px, py), (640, py), (640, py + 10))
y0, y1 = res_v(640, py + 10, "R3", "2.0k")
gnd(640, y1)
gx, gy = tp["GND"]
wire((gx, gy), (680, gy), (680, gy + 30)); gnd(680, gy + 30)

for i, cc in enumerate(["CC1", "CC2"]):
    cx, cy = jp[cc]
    x0, x1 = res_h(cx + 48, cy, f"R{i+1}", "5.1k")
    wire((cx, cy), (x0, cy))
    wire((x1, cy), (x1 + 12, cy)); gnd(x1 + 12, cy)
gx, gy = jp["GND"]
wire((gx, gy), (260, gy)); gnd(260, gy)
sx, sy = jp["SHELL"]
wire((sx, sy), (240, sy), (240, gy)); dot(240, gy)

# ---------------- BT1 + protection (top right) ----------------------
bt, bth = ic(1300, 110, 120, "BT1", "18650 holder",
             [("", "B+"), ("", "B-")], [], pitch=40)
bpx, bpy = bt["B+"]
bnx, bny = bt["B-"]

bx, by = tp["BAT"]
wire((bx, by), (bpx, bpy))
text(1120, 142, "BAT", 11, bold=True)

# DW01A: control pins east so the CELL_N bus can run underneath
dw, dwh = ic(1480, 110, 150, "U3", "DW01A SOT23-6",
             [(5, "VCC"), (2, "CS")],
             [(1, "OD"), (3, "OC"), (4, "TD"), (6, "GND")])
dvx, dvy = dw["VCC"]                       # (1458,136)
x0, x1 = res_h(1250, 96, "R7", "470R")
wire((1230, bpy), (1230, 96), (x0, 96)); dot(1230, bpy)
wire((x1, 96), (1440, 96), (1440, dvy), (dvx, dvy))
csx, csy = dw["CS"]                        # (1458,162)
wire((csx, csy), (1446, csy), (1446, csy + 8))
y0, y1 = res_v(1446, csy + 8, "R8", "1.0k")
gnd(1446, y1)

# CELL_N bus at y=264: B-  ->  U4.CELL_N  ->  U3.GND
wire((bnx, bny), (1268, bny), (1268, 264), (1670, 264))
text(1290, 256, "CELL_N", 11, bold=True)
dgx, dgy = dw["GND"]                       # (1652,214)
wire((dgx, dgy), (1668, dgy), (1668, 264)); dot(1668, 264)

fs, fsh = ic(1480, 360, 150, "U4", "FS8205A TSSOP-8",
             [("", "GATE_A"), ("", "GATE_B"), ("", "CELL_N")],
             [("", "PACK_GND")], pitch=30)
odx, ody = dw["OD"]
gax, gay = fs["GATE_A"]
wire((odx, ody), (1704, ody), (1704, 310), (1416, 310), (1416, gay), (gax, gay))
ocx, ocy = dw["OC"]
gbx, gby = fs["GATE_B"]
wire((ocx, ocy), (1688, ocy), (1688, 330), (1432, 330), (1432, gby), (gbx, gby))
cnx, cny = fs["CELL_N"]
wire((cnx, cny), (1400, cny), (1400, 264)); dot(1400, 264)
pgx, pgy = fs["PACK_GND"]
wire((pgx, pgy), (pgx + 16, pgy)); gnd(pgx + 16, pgy)
tdx, tdy = dw["TD"]
text(tdx + 6, tdy + 4, "n.c.", 11)
text(1300, 520, "C3 100nF across U3.VCC-CELL_N", 10.5)
text(1300, 536, "(datasheet reference circuit)", 10.5)

# ---------------- /CHRG: status LED + IO2 sense ---------------------
hx, hy = tp["/CHRG"]                       # (912,180)
wire((hx, hy), (946, hy))
dot(946, hy)
dot(490, vy)
wire((490, vy), (490, 96), (500, 96))
x0, x1 = res_h(500, 96, "R5", "2.0k")
d0, d1 = diode_h(x1 + 12, 96, "LED10", "red", led=True)
wire((x1, 96), (d0, 96))
wire((d1, 96), (946, 96), (946, hy))
text(640, 112, "lit while charging", 10)
wire((946, hy), (946, 700), (1060, 700), (1060, 848))
dot(946, 560)
wire((946, 560), (990, 560))
y0, y1 = res_v(990, 494, "R6", "100k")
wire((990, 560), (990, y1))
rail(990, y0, "3V3")
text(1078, 692, "IO2: /CHRG sense", 10)

# ---------------- BAT down to boost + battery divider ---------------
dot(1010, by)
wire((1010, by), (1010, 346), (248, 346), (248, 456))
text(600, 338, "BAT", 11, bold=True)

dot(820, 346)
wire((820, 346), (820, 354))
y0, y1 = res_v(820, 354, "R15", "100k")
wire((820, y1), (820, 422))
dot(820, 414)
y0, y1 = res_v(820, 422, "R16", "100k")
gnd(820, y1)
wire((820, 414), (1080, 414), (1080, 804), (1022, 804))
text(880, 406, "VBAT/2 -> IO1 (battery gauge)", 10)
dot(864, 414)
wire((864, 414), (864, 422))
cy0, cy1 = cap_v(864, 422, "C13", "100n")
gnd(864, cy1)

# ---------------- U5 boost + U6 LDO ---------------------------------
mt, mth = ic(320, 430, 150, "U5", "MT3608 SOT23-6",
             [(5, "VIN"), (4, "EN"), (2, "GND")],
             [(1, "SW"), (3, "FB")])
mvx, mvy = mt["VIN"]                       # (298,456)
wire((248, 456), (mvx, mvy))
dot(270, 456)
wire((270, 456), (270, 464))
cy0, cy1 = cap_v(270, 464, "", "")
gnd(270, cy1)
text(270, 524, "C4 10µF", 10.5, anchor="middle")
mex, mey = mt["EN"]
wire((mex, mey), (284, mey), (284, 456)); dot(284, 456)
mgx, mgy = mt["GND"]
wire((mgx, mgy), (306, mgy), (306, mgy + 26)); gnd(306, mgy + 26)
dot(258, 456)
wire((258, 456), (258, 412), (268, 412))
lx0, lx1 = inductor_h(268, 412, "L1", "22µH")
swx, swy = mt["SW"]                        # (492,456)
wire((lx1, 412), (500, 412), (500, swy), (swx, swy))
dot(500, swy)
d0, d1 = diode_h(500, swy, "D1", "SS34")
wire((d1, swy), (620, swy))
dot(620, swy)
cy0, cy1 = cap_v(620, swy + 8, "C5", "22µF")
gnd(620, cy1)
wire((620, swy), (660, swy))
rail(660, swy, "5V")
fbx, fby = mt["FB"]                        # (492,482)
wire((fbx, fby), (520, fby), (520, 530))
dot(520, 530)
x0, x1 = res_h(404, 530, "R10", "15k")
wire((x1, 530), (520, 530))
wire((x0, 530), (392, 530)); gnd(392, 530)
x0, x1 = res_h(532, 530, "R9", "110k")
wire((520, 530), (x0, 530))
wire((x1, 530), (620, 530), (620, swy + 32))
text(300, 588, "Vout = 0.6V x (1 + 110/15) = 5.0V", 10.5)

ap, aph = ic(620, 560, 150, "U6", "AP2112K-3.3 SOT-25",
             [(1, "VIN"), (3, "EN"), (2, "GND")],
             [(5, "VOUT"), (4, "NC")])
avx, avy = ap["VIN"]                       # (598,586)
wire((avx, avy), (556, avy))
rail(556, avy, "5V")
dot(572, avy)
wire((572, avy), (572, avy + 8))
cy0, cy1 = cap_v(572, avy + 8, "", "")
gnd(572, cy1)
text(558, avy + 24, "C6 10µF", 10.5, anchor="end")
aex, aey = ap["EN"]
wire((aex, aey), (586, aey), (586, avy)); dot(586, avy)
agx, agy = ap["GND"]
wire((agx, agy), (590, agy), (590, agy + 26)); gnd(590, agy + 26)
aox, aoy = ap["VOUT"]                      # (792,586)
wire((aox, aoy), (900, aoy))
rail(900, aoy, "3V3")
dot(810, aoy)
wire((810, aoy), (810, aoy + 8))
cy0, cy1 = cap_v(810, aoy + 8, "C7+C8", "")
gnd(810, cy1)
text(810, aoy + 78, "10µ+100n", 10.5, anchor="middle")
dot(876, aoy)
y0, y1 = res_v(876, aoy + 12, "R11", "2.0k")
wire((876, aoy), (876, y0))
add(f'<path d="M876,{y1} l0,6 l-8,0 l8,14 l8,-14 l-8,0" fill="none" stroke="black" stroke-width="1.6"/>')
line(868, y1 + 20, 884, y1 + 20, 2)
wire((876, y1 + 20), (876, y1 + 28))
gnd(876, y1 + 28)
text(858, y1 + 38, "LED11 green", 10.5, anchor="end")

# ---------------- U1 ESP32-S3 module (center) -----------------------
u1, u1h = ic(700, 760, 300, "U1", "ESP32-S3-WROOM-1-N8",
             [("", "3V3"), ("", "GND"), ("", "EN"), ("", "IO0"),
              ("", "IO19/USB_D-"), ("", "IO20/USB_D+"),
              ("", "TXD0/IO43"), ("", "RXD0/IO44")],
             [("", "IO1"), ("", "IO2"), ("", "IO4"), ("", "IO5"),
              ("", "IO6"), ("", "IO7"), ("", "IO8"), ("", "IO9"),
              ("", "IO10"), ("", "IO11"), ("", "IO12"), ("", "IO13"),
              ("", "IO14"), ("", "GND ")], pitch=44)
text(850, 760 + u1h + 24,
     "strapping/PSRAM pins IO3, IO45, IO46, IO35-37: leave unconnected",
     10.5, anchor="middle")

x, y = u1["3V3"]                           # (678,804)
wire((x, y), (560, y))
rail(560, y, "3V3")
dot(620, y)
wire((620, y), (620, y + 8))
cy0, cy1 = cap_v(620, y + 8, "", "")
gnd(620, cy1)
text(605, y + 24, "C9+C10 10µ,100n", 10.5, anchor="end")
x, y = u1["GND"]
wire((x, y), (652, y)); gnd(652, y)
x, y = u1["GND "]
wire((x, y), (x + 24, y)); gnd(x + 24, y)

# EN row: pull-up, C11, SW3; Q2 collector joins from below
enx, eny = u1["EN"]                        # (678,892)
wire((enx, eny), (340, eny))
dot(440, eny)
y0, y1 = res_v(440, eny - 66, "R12", "10k")
wire((440, eny), (440, y1))
rail(440, y0, "3V3")
b0, b1 = button(296, eny, "SW3 RESET")
wire((b0[0], eny), (286, eny)); gnd(286, eny)

# IO0 row: SW2; Q1 collector joins from below
iox, ioy = u1["IO0"]                       # (678,936)
wire((iox, ioy), (340, ioy))
b0, b1 = button(296, ioy, "SW2 BOOT")
wire((b0[0], ioy), (286, ioy)); gnd(286, ioy)

# ---------------- U7 CH340C + auto-download -------------------------
cp, ch = ic(240, 990, 150, "U7", "CH340C SOP-16",
            [(5, "UD+"), (6, "UD-"), (16, "VCC"), (4, "V3"), (1, "GND")],
            [(2, "TXD"), (3, "RXD"), (13, "DTR#"), (14, "RTS#"), ("", "NC*")])
text(315, 990 + ch + 16, "* pins 7,8,9,10,11,12,15 n.c.", 10, anchor="middle")

dx, dy = jp["D+"]
qx, qy = cp["UD+"]
wire((dx, dy), (224, dy), (224, qy), (qx, qy))
dx, dy = jp["D-"]
qx, qy = cp["UD-"]
wire((dx, dy), (236, dy), (236, qy), (qx, qy))
vcx, vcy = cp["VCC"]
wire((vcx, vcy), (150, vcy))
rail(150, vcy, "3V3")
v3x, v3y = cp["V3"]
wire((v3x, v3y), (210, v3y), (210, vcy)); dot(210, vcy)
dot(170, vcy)
wire((170, vcy), (170, vcy + 8))
cy0, cy1 = cap_v(170, vcy + 8, "C12", "100n")
gnd(170, cy1)
cgx, cgy = cp["GND"]
wire((cgx, cgy), (200, cgy), (200, cgy + 20)); gnd(200, cgy + 20)

ctx, cty = cp["TXD"]
tx0, ty0 = u1["TXD0/IO43"]
wire((ctx, cty), (560, cty), (560, ty0), (tx0, ty0))
crx, cry = cp["RXD"]
rx0, ry0 = u1["RXD0/IO44"]
wire((crx, cry), (540, cry), (540, ry0), (rx0, ry0))

dtx, dty = cp["DTR#"]
rtx, rty = cp["RTS#"]
q1B, q1C, q1E = npn(470, 1190, "Q1 S8050")   # C -> IO0
q2B, q2C, q2E = npn(396, 1300, "Q2 S8050")   # C -> EN
# DTR -> R13 -> Q1.B ; DTR -> Q2.E
wire((dtx, dty), (430, dty), (430, 1102), (340, 1102), (340, 1190), (368, 1190))
x0, x1 = res_h(368, 1190, "R13", "12k")
wire((x1, 1190), q1B)
dot(340, 1190)
wire((340, 1190), (340, 1268), (280, 1268), (280, 1350), (q2E[0], 1350), q2E)
# RTS -> R14 -> Q2.B ; RTS -> Q1.E
wire((rtx, rty), (434, rty), (434, 1240), (300, 1240), (300, 1300), (314, 1300))
x0, x1 = res_h(314, 1300, "R14", "12k")
wire((x1, 1300), q2B)
dot(434, 1240)
wire((434, 1240), (470, 1240), (470, q1E[1]), q1E)
wire(q1C, (470, ioy)); dot(470, ioy)
wire(q2C, (396, 1270), (396, eny)); dot(396, eny)
# C11 on the EN net (hung from the Q2 collector run, clear of IO0 row)
dot(396, 962)
wire((396, 962), (436, 962), (436, 970))
cy0, cy1 = cap_v(436, 970, "C11", "1µF")
gnd(436, cy1)
text(60, 1400, "auto-download: DTR/RTS cross-pair drives EN + IO0", 10.5)
text(60, 1416, "(idf.py flash over USB-C, no buttons)", 10.5)

# ---------------- J2 USB-A + ESD (bottom left) ----------------------
ja, jah = ic(60, 1470, 130, "J2", "USB-A THT",
             [], [("1", "VBUS"), ("2", "D-"), ("3", "D+"), ("4", "GND")],
             pitch=34)
es, esh = ic(400, 1470, 160, "U8", "USBLC6-2SC6",
             [("", "IO1"), ("", "IO2"), ("", "GND")],
             [("", "IO1'"), ("", "IO2'"), ("", "VBUS")], pitch=34)
jvx, jvy = ja["VBUS"]
x0, x1 = res_h(236, jvy, "F1", "500mA PTC")
wire((jvx, jvy), (x0, jvy))
wire((x1, jvy), (322, jvy))
rail(322, jvy, "5V")
jdx, jdy = ja["D-"]
e1x, e1y = es["IO1"]
wire((jdx, jdy), (330, jdy), (330, e1y), (e1x, e1y))
jdx, jdy = ja["D+"]
e2x, e2y = es["IO2"]
wire((jdx, jdy), (348, jdy), (348, e2y), (e2x, e2y))
jgx, jgy = ja["GND"]
wire((jgx, jgy), (jgx + 20, jgy)); gnd(jgx + 20, jgy)
egx, egy = es["GND"]
wire((egx, egy), (368, egy), (368, egy + 26)); gnd(368, egy + 26)
evx, evy = es["VBUS"]
wire((evx, evy), (evx + 26, evy))
rail(evx + 26, evy, "5V")
o1x, o1y = es["IO1'"]
m1x, m1y = u1["IO19/USB_D-"]
wire((o1x, o1y), (614, o1y), (614, m1y), (m1x, m1y))
o2x, o2y = es["IO2'"]
m2x, m2y = u1["IO20/USB_D+"]
wire((o2x, o2y), (632, o2y), (632, m2y), (m2x, m2y))
text(60, 1650, "keyboard / dongle-provisioning port — keep D+/D- matched,", 10.5)
text(60, 1666, "< 30mm over ground; same port flashes dongles + the hub ROM", 10.5)

# ---------------- LED bank + MODE (right of module) -----------------
for i in range(10):
    sxp, syp = u1[f"IO{4+i}"]
    wire((sxp, syp), (1150, syp))
    x0, x1 = res_h(1150, syp, f"R2{i}", "1.0k")
    d0, d1 = diode_h(x1 + 6, syp, f"LED{i}", "", led=True)
    wire((x1, syp), (d0, syp))
    wire((d1, syp), (d1 + 12, syp)); gnd(d1 + 12, syp)
text(1330, 870, "slot LEDs: silkscreen digits 0-9", 10.5)
mx, my = u1["IO14"]
wire((mx, my), (1150, my))
b0, b1 = button(1150, my, "SW1 MODE")
wire((b1[0], my), (b1[0] + 12, my)); gnd(b1[0] + 12, my)
text(1260, my + 4, "(internal pull-up in firmware)", 10)
text(1090, 798, "VBAT/2", 10)

# ---------------- notes ---------------------------------------------
ny = 1720
text(60, ny, "NOTES", 14, bold=True)
notes = [
    "1. Only GND / 3V3 / 5V use power symbols; every other connection is drawn. Crossings without a dot are not connected.",
    "2. Firmware pin map (locked, ui_headless.c): LEDs IO4-13, MODE IO14, USB IO19/20, VBAT/2 IO1, /CHRG IO2, UART0 IO43/44.",
    "3. Layout: module antenna overhangs the board edge (no copper under it, 18650 cell >= 10mm away); boost loop L1/D1/C5 tight,",
    "   SW node away from USB D+/D- and the IO1 divider; CC resistors close to J1. TP4056 + FS8205A want copper pour for heat.",
    "4. Charge current 500mA (R3 2.0k). Never substitute an IP5306-style powerbank IC (auto-off at light load kills an idle hub).",
    "5. Verify each footprint's pin numbering against its datasheet during capture. Full BOM + netlist: SCHEMATIC.md, same folder.",
]
for i, n in enumerate(notes):
    text(60, ny + 24 + i * 20, n, 11)
text(1660, ny + 144, "espkvm hub v1.0 - 2026-07-02 - MIT licensed", 11.5,
     anchor="end", bold=True)

add("</svg>")

with open("schematic.svg", "w", encoding="utf-8") as f:
    f.write("\n".join(E))
print(f"wrote schematic.svg ({len(E)} elements)")
