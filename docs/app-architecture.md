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

Computed while a program loads, from the same ops the run will send: each line segment gets
the planner's own geometry (`motion_plan_setup`: per-axis speed caps incl. the FPGA's sustained
rate, acceleration, the S-curve's average acceleration) and corner limit
(`motion_corner_limit`). A backward + forward pass gives every segment's entry speed, and a
trapezoid gives its time. Every non-motion op stops the motion first, as in the planner. Segments are planned in
windows of 2048 (256 carried over), so memory stays flat. Time is kept per line, cumulative,
at feed overrides 100/150/200/300 % (`program_t.est_cum`). Below 100 % it is scaled by
1/override; between levels it is interpolated in 1/override. Both over-estimate, because time is convex in
1/override. Rapids use min(feed, 100). Remaining = total − cumulative(current line), per JOB part
and tool segment for multi-part jobs. Margin: ×1.05 + 1 s, because the model alone is 1–4 %
short of the executor (slice and S-curve ends at each start/stop), measured in
`tests/test_estimate.c`. Tool changes by hand count 0 s.

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
