# Pandora — replacement operator app for the DDCS V4.1

A single C program for the controller (ARMv5, static, ~1.4 MB) that replaces the vendor
`ddcsv4.out`: G-code, homing, zeroing/touch-off, work and machine coordinates, multi-part
jobs, resume-from-line, overrides, settings, the panel UI on the 1024×600 LCD, the
18 keys + LEDs, the optional MPG pendant, and the Ethernet API. The same binary also
runs on a PC against a register-level FPGA simulator, with the panel in a browser.

Design: [../docs/app-architecture.md](../docs/app-architecture.md) ·
operator model: [../docs/operator-procedures.md](../docs/operator-procedures.md) ·
API: [../docs/ethernet-api.md](../docs/ethernet-api.md)

## Build (WSL / Linux)

Needs `gcc`, `make`, `python3`, and for the controller `gcc-arm-linux-gnueabi`
(+ `qemu-user` to run the ARM tests). From Windows, prefix with
`wsl -d Ubuntu-26.04 -- sh tools/wsl-make.sh` instead of `make`.

```bash
make test        # 611 checks: FPGA layer vs the vendor's recorded register writes,
                 # G-code, motion vs simulated FPGA, state machine, UI
make host        # build/host/pandora   (PC, simulator)
make arm         # build/arm/pandora    (controller, static ARMv5)
make test-arm    # the same tests, ARM build, under qemu-arm
```

The build id on the panel (8th soft-key box, above GOTO 0) is `$PIPELINE_ID` when set by
CI, otherwise `local-<n>` from a counter in `build/buildno`; the date is the build date.

## Run on the PC (simulator)

```bash
./build/host/pandora --programs programs --data build/run --port 8090
```

Open `http://localhost:8090/`: the physical panel with the app's real framebuffer in the
LCD. Click keys or use the keyboard (1–7, S, M, T, A, P, arrows, Esc, Enter; pendant
`[` `]` and Space). The "Simulated machine" card trips switches, presses E-stops and
can switch on the **strict done-latch** (the real FPGA's still-unsolved behaviour).
`python tools/grab.py http://localhost:8090 shot.png` saves the screen.

## Run on the controller

Tested on the unit 2026-10-18 (real FPGA, no motors): see [../docs/fpga-motion.md](../docs/fpga-motion.md).
The unit has no `scp`: copy with `ssh … "cat > /ddcsv4/pandora" < build/arm/pandora`.
A power cycle brings the vendor app back (rcS starts it). Order of first tests over SSH ([../docs/ssh-access.md](../docs/ssh-access.md)):

1. Copy the binary (above).
2. Stop the vendor app: `killall ddcsv4.out` (a power cycle brings it back — rcS starts it).
3. **Keys + LCD only, machine simulated**: `/ddcsv4/pandora --sim` — nothing can move;
   checks the TM1638 keys/LEDs, the framebuffer and the browser panel on the real unit.
4. Real I/O: `/ddcsv4/pandora` (uses the vendor `motiondev.ko` already loaded at boot).
   Inputs, outputs, spindle 0–10 V, pendant and E-stop are on proven registers. **Motion is
   limited by the open FPGA items below** — test with the motors unpowered or free first.

Data files: `/ddcsv4/pandora.cfg` (settings), `/ddcsv4/pandora-work.cfg` (G54–G59,
tools), `…work.cfg.pos` (last position). Programs are read from `/local` (the CNCDISK
share) and `/udisk-sda1`, `/udisk-sda` (USB).

## Open hardware items (from the docs, honest status)

| Item | Effect on the real machine | Where |
|---|---|---|
| ~~Move-done acknowledge~~ **solved**: `0x67` is a motion watchdog. Motion verified step-exact on the unit (0 faults, Reset overrun ≤ 1 mm) | — | [docs/fpga-motion.md](../docs/fpga-motion.md) |
| Bursts are stop-go (only into an idle axis); no motor tested yet | Check smoothness with a real stepper | `core/motion.c` emit() |
| FPGA ceiling ~40 000 steps/s (1200 mm/min here) | Max speed | `hal/fpga.h` FPGA_MAX_SPS |
| `0x04 = 4` mid-move holds the axis until power-off | Never used by the app | `hal/fpga.c` fpga_stop() |
| Vendor home offset `#214/#215` semantics | Ours: "position at switch" (default 0) + back-off | `core/settings.c` |
| Limit level "High" (NC) with no switches wired (desk unit) | Every limit reads active → LIMIT at start-up; set *Limit/home active when open* OFF on the desk | Settings › Homing & limits |
| Probe input not wired | Probe routines refuse | — |

## Layout

```
src/common.*        types, clock (real or virtual for tests)
src/hal/            bus (vendor ioctl | simulator), fpga (typed registers), panel (TM1638 | sim), display (fb0 | RAM)
src/sim/            register-level FPGA + machine model
src/core/           settings table, G-code interpreter, motion, programs, state machine + controller
src/ui/             RGB565 renderer, baked fonts, six screens, digit editor, toolpath view, pendant
src/net/            non-blocking HTTP server, API, embedded browser panel (web/panel.html)
tests/              layer tests (run natively and under qemu-arm)
tools/              genfont.py (fonts → C), embed.py, grab.py, wsl-make.sh
programs/           demo G-code for the simulator
```
