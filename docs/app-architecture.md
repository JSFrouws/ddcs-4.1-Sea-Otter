# Device app architecture (`app/`)

_2026-10-18. The replacement for `ddcsv4.out`, built and tested; not yet run on the unit.
Code: [../app/](../app/README.md). Operator model: [operator-procedures.md](operator-procedures.md)._

## Decisions

| Question | Decision | Why |
|---|---|---|
| Language / UI toolkit | **C, own RGB565 renderer** (no Qt, no LVGL, no Python) | ARM926 at 454 MHz with **no FPU** and 58 MB RAM (≈16 MB free with the vendor app running). One static binary, no libraries to port to Linux 2.6.35 / glibc 2.13 |
| FPGA access | **Reuse the vendor's `motiondev.ko`** (already loaded by `rcS`) through its ioctls `0x4620/0x4621` (register write/read), `0x4627` (spindle PWM), `0x4623/24` (beep) | Proven on this unit, CRC-valid for its kernel. Our rebuilt `motiondrv.ko` still can't load without the BSP's `Module.symvers` ([motiondev-ko.md](motiondev-ko.md)) |
| Keys / LEDs | **TM1638 from userspace via `/dev/mem`**, same code path as `mockup/ioctl-probe/tm1638read.c` | `MOTION_IOC_KEY` returns nothing ([ui-controls-map.md](ui-controls-map.md)); the direct scan is proven. Only compiled-in for ARM |
| Display | `/dev/fb0` mmap, RAM back buffer, copy **changed rows only** | 1024×600×16 = 1.2 MB per full copy; most frames change a few DRO digits |
| Motion | **Software interpolation**: every 10 ms the planner advances along the line (trapezoid, one-segment lookahead), each axis gets one **step burst** | The burst command (vendor MPG-jog sequence) is the only motion command proven on hardware. The FPGA's own coordinated block (`0x50..0x60`) isn't decoded |
| Concurrency | **Single thread**, one loop: 10 ms tick → inputs, state machine, program feeder, motion, outputs; keys at 50 Hz; render ≤ 10 fps when something changed; HTTP polled non-blocking in the idle time | No locks, deterministic, and a slow network client can't delay a motion slice |
| Settings | One descriptor table `SETTINGS[]` (key, group, label, type, limits, decimals, "idle only", "panel only") | The settings screen, the API and the file format can't drift apart |
| Safety model | One dispatcher `act()` + state table `ALLOW[state]` | No input path (key, pendant, API, internal event) can reach motion without the same guard |

## Layers and their test counterparts

```
 panel keys ─┐   pendant ─┐   HTTP API ─┐
             ▼            ▼             ▼
        ui/ (screens, editor, view) ─── act() ──► core/machine.c  ALLOW[state] / NEXT
                                                   │  core/ctl_tick.c: inputs, feeder, events, outputs
                                                   ▼
                       core/gcode.c ──ops──► core/motion.c (planner, homing, bursts)
                                                   ▼
                                           hal/fpga.c (typed registers)
                                                   ▼
                              hal/bus.h ──► bus_ioctl.c (vendor motiondev.ko)  │  bus_sim.c ──► sim/fpga_sim.c
```

| Layer | Tested against | How |
|---|---|---|
| `hal/fpga.c` | **the vendor app itself** | The boot-time I/O setup (64 writes) and a jog burst (10 writes) are compared write-for-write with `raw/2026-10-16_fpga-outputs/iotrace_run3.log` |
| `hal/fpga.c` | `sim/fpga_sim.c` | Bursts move the simulated motor the right way (incl. X's DIR invert + reversed wiring), inputs/pendant/wheel decode, outputs, and the strict done-latch |
| `core/gcode.c` | expected coordinates | WCS, G53, G91, inches, arcs (IJ and R, radius error < 1e-9), canned cycles G73/G81/G82/G83/G85/G89 (G98/G99, modal repeats, G80, 1000-peck holes, resume after a cycle), G10 L20, M-codes, error lines, resume seek |
| `core/motion.c` | simulated FPGA | Position exact to the step, speed ≤ feed, hold/resume, feed override, pendant nudges, 100 kHz ceiling |
| `core/machine.c` | simulated machine | Every directional command refused in RUN/HOLD/HOMING (panel and API), homing on the simulated switches, soft limits, touch-off ±Ø/2 then 0, goto-zero, per-direction limit, pendant E-stop, TryCut Z clamp, Simulate (zero pulses), stop + resume from a line, multi-part job, remote-start 403 |
| `ui/` | controller + snapshots | Mode/Page R/Tab/ALT routing, digit editor (100.00 / 900 rule), view clamps, decimals, pendant nav/change/quick-press, 11 screens rendered to `build/snapshots/` |

`make test` runs all of them natively, `make test-arm` runs the ARM build under qemu: 611/611.

## The simulator

`sim/fpga_sim.c` answers the FPGA's register addresses: banked per-axis registers via
`0x00`, bursts started by the `0x04` go edge, busy in `0x01` bit 9, inputs `0x37/0x38`
from a machine model (one shared NC switch per axis at the X/Y negative and Z top ends,
like this machine; E-stop button on IN01), pendant `0x3D` gated by the dead-man, wheel
`0x0E`, outputs `0x3E`, PWM. Unknowns are explicit options, not hidden guesses: a burst's
pulse rate (spread over 10 ms, capped at 100 kHz), and the `strict_latch` that reproduces
the real FPGA ignoring moves after the first one.

## Rendering budget

Fonts are baked at build time (Barlow Semi Condensed + JetBrains Mono, OFL; DejaVu for
arrows/dashes) as 4-bit alpha, blended into RGB565. Gradients are short (≤ 140 px) so
16 bpp doesn't band. A frame is only rendered when a signature of visible state changes,
at most 10 fps while moving, and only changed rows reach `/dev/fb0`. The toolpath view is
lines only (2D plan or isometric wireframe, turn/tilt clamped at the side and top views),
decimated to ≥ 1 px steps, preview capped at 120 000 vertices.

Measured on the unit with real CAM programs (2026-10-18):

| Program | Toolpath draw per frame | Motion time lost |
|---|---|---|
| 4 928 lines, before the fixes | 45–55 ms (longer than a 40 ms motion slice) | 1.6–3.5 s **per minute** |
| same, with `ui_yield` | same draw time | **0** |
| 64 396 lines, with `ui_yield`, no cache | **312 ms** (panel ~3 fps, keys sluggish) | 0 |
| 64 396 lines, with the view cache | **~50 ms whole frame** (draw ~26 ms) | **0** over 761 s |

Two mechanisms make that work:

- **`ui_yield`** (set by `main.c` to `slice_if_due`): a long draw lets a motion slice run
  every 512 vertices, so drawing can never starve motion.
- **View cache** (`ui_view.c`): the panel background, stock box, triad and toolpath are drawn
  once into an off-screen copy and blitted each frame. While running, only the segments that
  became *done* since the last frame are painted on. Anything that changes the picture (view
  turn/zoom, program, panel focus, banner height) rebuilds it; the tool marker and labels are
  never cached.
- **CPU saver** view (F4) skips the toolpath entirely and redraws at most once a second while
  moving.

Still open: loading a 64 000-line file takes **4.3 s** and blocks the loop (fine while idle,
and refused while a program or move runs - but a jog in progress would be stalled by it).

## Run-time estimate (`core/estimate.c`)

Worked out in the **background** after a program loads (`est_background` in `ctl_tick`: 3 ms
per tick idle, 1 ms while running). Done during the load, it made the 64 396-line propellor
file load in 17 s instead of 3 s on the unit, which has no FPU. Now that file loads in 3.2 s
(it took 4.3–4.6 s before), and the panel shows "…" until the first figure is there.

1. **Model** (seconds): every line segment gets the planner's own geometry
   (`motion_plan_setup`) and corner limit (`motion_corner_limit`). A backward + forward pass
   (speeds squared: one square root per segment) gives every segment's entry speed, and a trapezoid gives its time.
   Every non-motion op stops the motion first. Kept per line at 100 % and 200 % feed (`est_cum`).
   Measured against the executor it is off by −0.1 … +0.3 s on small programs, and 1–4 % short on
   whole ones, so it is shown with ×1.05 + 1 s until step 2 is done.
2. **Exact** (seconds to minutes; the 3.4-hour propellor file takes a few minutes): the
   program is replayed through a second, dry copy of the real executor (`motion_t`, as
   Simulate runs), fed as `ctl_tick` feeds it, ticked in 10 ms steps. The line it executes at
   each tick gives `exact_cum`. Pauses continue at once.
3. **Overrides**: the exact time is scaled by the model's factor (time at the override /
   time at 100 %). Below 100 % that is 1/override; between 100 and 200 % it is interpolated in
   1/override; above 200 % the 200 % time is used. All three err long. Rapids use min(feed, 100).
4. **The start**: the estimate assumes work zero at safe Z, but a run starts where the machine is,
   and every move keeps the axes it doesn't name. So the program's ops are generated from both
   starts until they end in the same place (X Y Z; A is ignored, because a program that never
   moves A never meets). Both beginnings are replayed, and the difference is added. While a
   run is still in that beginning, its own replayed times are used.
5. **Running**: remaining = from the current line on, and the line in progress counts at least
   what the move in progress still has to go. Multi-part jobs: per tool segment and part.
   Margin once exact: ×1.003 + 0.05 s; figures are rounded up.

**The executor had to change for this to be exact on the unit**: its speed and braking
decisions were taken once per controller tick, and the unit's ticks are uneven (a screen
redraw makes a 30–40 ms tick). The real `Sign_rosette.nc` ran 2.4 % slower with a 32 ms tick
every 18 and 11.7 % slower with a 40 ms tick every 9 than at steady 10 ms ticks. The machine
braked early whenever the screen redrew. `motion_tick` now runs the path in steps of at most
10 ms (still one burst per tick), and the time no longer depends on the tick pattern
(±0.2 %, `tests/test_estimate.c`).

Measured on the unit (2026-10-25):

| Program | Mode | Run | Estimate |
|---|---|---|---|
| Sign_rosette.nc (1208 lines) | Run (FPGA stepping) | 260.87 s | 261 s (+0.1 %) |
| Sign_rosette.nc | Simulate | 244.97 s | 245 s (+0.0 %) |
| 410-line test, started 120 mm away and 40 mm below work zero | Run | 29.15 s | 29 s (−0.5 %, whole seconds) |

Before these fixes: the same 410-line test showed 17 s for a 29 s run (−42 %, the start),
and the rosette 261 s for 265 s (−1.6 %, the uneven ticks). Reset still stops after 0.19 mm at
1200 and 3000 mm/min, counters = commanded steps (`tools/device_stop_test.sh`).
Tool changes by hand count 0 s.

## Controller quirks the code works around

- **`opendir()` fails on the controller** with `EINVAL` (the static, modern C library vs the
  2.6.35 kernel; measured 2026-10-22), while plain `open(O_DIRECTORY)` + the `getdents64`
  system call work. Every directory listing (Files page, macro folders) therefore goes
  through `dir_list()` in `src/common.c`. Before this fix the Files page listed nothing on
  the real unit (programs were only loaded by name over the API).

## Open items before the first real cut

The former blocker (the "done latch") is solved: `0x67` is a motion watchdog. With the fixes
in [fpga-motion.md](fpga-motion.md) the app ran step-exact on the real FPGA (0 position
faults, Reset overrun ≤ 1 mm; controlled stop ≤ 1 mm at 16 mm/s). Still to do: test with a real motor (bursts are stop-go),
homing on real switches, and decoding the coordinated block interface (`0x50…0x60`).
