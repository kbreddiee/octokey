# espkvm user guide — pairing & switching

## Pairing a dongle

1. **Hub:** hold the encoder button for **3 seconds** → the OLED shows
   `PAIRING MODE`. The window stays open for 30 s (click to cancel).
2. **Dongle:** plug it into the target machine, then **short-press the
   BOOT button** (the button marked `0` on the S2 Mini). Its LED switches
   to a fast blink while it talks to the hub.
3. Within a second the hub shows `Paired PC n` and the dongle LED goes
   **solid**. The dongle is assigned the lowest free slot (0–9) and both
   sides remember each other permanently (survives power loss).

Re-pairing an already-known dongle (e.g. after re-flashing the hub… or
just for fun) keeps its slot number and display name.

During pairing the two devices perform an ephemeral Diffie–Hellman
exchange and derive a fresh per-dongle encryption key — nothing secret
travels over the air. Details in [SECURITY.md](SECURITY.md).

## Switching machines

**Encoder:** rotate to move the cursor across paired slots (the bottom
strip shows all ten), click to switch. The OLED shows the active slot's
number, name and link status.

**Hotkey, from the keyboard itself:** double-tap **Right-Ctrl**, then press
a digit — `3` jumps to slot 3, `0` to slot 0. `Esc` cancels; so does doing
nothing for 2 s. The header bar inverts while espkvm is waiting for the
digit. The chord is intercepted at the hub and **never reaches the target
machine**; the timing windows are tunable in `menuconfig`.

On every switch the hub sends a *release-all* to the machine you're
leaving, so a key held during the switch can't stay stuck there. The
dongles also release everything on their own if the hub goes silent for
1.5 s while something is held — yanking the hub's power mid-keypress is
handled.

If the hub reboots it comes back on the last active slot.

## Dongle LCD (LILYGO T-Dongle-S3)

Dongles with the T-Dongle-S3's screen show their **slot number as a huge
digit**, so you always know which digit to press after the Right-Ctrl
double-tap. The screen turns **green with "ACTIVE"** on whichever machine
is currently selected, and warns with `hub offline` / `USB not up` when
something's wrong. S2 Mini dongles convey the same states with their LED
(see below).

## Link status

The bottom strip on the OLED shows one cell per slot: `-` = empty,
digit = paired, inverted digit = active. A dot above a digit means that
dongle is currently **offline** (machine off, out of range...). Status
comes from real radio ACKs, refreshed every 0.5 s for the active slot and
every 2 s for the rest. Input to an offline machine is dropped, never
queued — when the machine comes back you won't get a burst of stale
keystrokes.

## Forgetting a dongle

Double-click the encoder → menu → rotate to `Forget <name>` → click.
The hub tells the dongle to factory-reset itself (if it's in range) and
deletes the slot either way.

## Factory reset

- **Dongle:** hold BOOT for 5 s → wipes its pairing, reboots unpaired
  (slow LED blink).
- **Hub:** `idf.py erase-flash` (wipes pairings, keys and the boot
  counter).

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Dongle LED blinks slowly, hub says nothing | Not paired: hub into pairing mode first, then press BOOT |
| `no kbd!` on the OLED | The hub doesn't see your keyboard — check the USB-A breakout wiring and that 5 V is actually present |
| Typing lands on the wrong machine | Look at the inverted digit on the strip — you probably switched with the hotkey without noticing |
| Slot shows offline but the PC is on | USB port may have powered down the dongle; try another port or disable USB selective suspend |
| Pairing times out | Both sides must be on the same Wi-Fi channel (`menuconfig → espkvm`, default 1); rebuild if you changed it on only one side |
| Media keys do nothing | Some keyboards send vendor-specific usages; open an issue with the descriptor dump from the hub's log |
