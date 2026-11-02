# Block mode: continuous motion through the FPGA's block queue — design

_2026-11-01. The decoded interface and the measurements are in
[fpga-motion.md](fpga-motion.md) "Block mode = continuous motion". This file is the
architecture the app builds on it. Status: implemented 2026-11-01 (cc64bbb, 2ef7448); the
first machine runs found the word-2 and full-queue traps and then the post-stream halt,
which turned out to be three missing parts of the FPGA initialisation (the vendor's long
pauses, 31a87e8; its reads, e6507bc; and its pace - every gap it leaves, the evening of
2026-11-02 - fpga-motion.md "INITIALISATION problem"). **Proven on the unit 2026-11-02**
(the burst-free build: 3 of 3 cold boots, 3 of 3 app restarts and `trace_square.nc` over
the API, queue 0 and no halt after each; motors disconnected, so smoothness by ear is
still to be judged). Replaces the burst
emitter for jog, wheel, programs and homing; **burst mode was removed 2026-11-02** (owner's
decision): blocks are the only motion path, and `motion.block_mode` is gone (an old saved
key is ignored on load)._

## The contract with the FPGA (what we rely on, all measured)

- A **block** is "n steps per axis at a rate", 16 words through ioctl `0x4622`
  (→ regs `0x50…0x5F`, latched by `0x66`). The FPGA **queues up to 256 blocks** and plays
  them **back to back with no gap**; the position counters count every step.
- Word layout: `w0` flags — bit 9 = direction of the *dominant* axis, bits 8/7/6 =
  directions of the *remaining* axes in index order, bits 1–2 = the dominant axis'
  index (X 0, Y 2, Z 4, A 6), bit 0 = feed (1) / rapid (0), bit 5 = 1; `w1 = 0x3020`;
  `w2` = sequence id (free); `w4` = **dominant rate word = steps/s ÷ 2**; `w6 = 0x0d05`;
  `w8` = dominant steps; `w10/w12/w14` = the remaining axes' steps in index order.
- `R 0x36` = blocks queued (`0xff` = full). An empty queue stops the axis; the next block
  resumes it. Direction may change from one block to the next.
- The `0x67` watchdog must keep being written or nothing plays.
- Before a stream the vendor writes, per axis, `W 00=a; W 42=0x30+a; W 00=0x0f`; after it
  `0x20+a`. We do the same (`fpga_block_mode`). Nothing else is needed.
- Status `0x01` while a block stream runs on channel 1 reads `0x2020`: bits 4+ch and
  12+ch — a different layout from burst mode (bits ch and 8+ch). `fpga_moving()` reads the
  block layout only since burst mode was removed.
- Open (to be measured): what a mid-stream `0x04 = 4` does in block mode; whether the mode
  write `0x20+a` flushes the queue.

## The time base belongs to the FPGA

In burst mode the planner was ticked by the wall clock and each slice became a burst
sized to it; every late slice was a gap and every early one a double. In block mode the
**FPGA's queue is the clock**: every block lasts exactly its planned time, back to back,
so the planner is advanced in **fixed 10 ms blocks** and the main loop's only job is to
**keep the queue at a target depth**. Wall-clock jitter (a 100 ms frame, an HTTP request)
is absorbed by the queue as long as it is refilled before it runs dry.

```
motion_tick(m, wall_dt):                     every wall tick (10 ms) and every ui_yield
    q = fpga_block_queue()                   blocks still queued
    while q < target && planner moves:       target = motion.queue_ms / 10 (+ frame top-up)
        plan one 10 ms block (run_lines / run_simple / run_home with dt = 0.010)
        emit_block(): one block carrying all four axes' step deltas; q++
    (nothing to move: one planner step per wall tick, no block — the wheel's
     averaging and the jog ramps keep wall time)
    (a dry run / Simulate: the planner follows wall_dt, steps are counted, nothing is sent)
```

- `motion_t.steps[]` = steps *sent* (ahead of the FPGA by ≤ the queue depth). The counters
  lag by that much; the position check at rest (queue empty, nothing moving) is unchanged.
- Latency of a jog key or wheel click = queue depth (`motion.queue_ms`, default 50 ms);
  stop latency the same plus the planned ramp.
- Before a frame the loop tops the queue up to the frame's length (`motion_feed_ahead`), so
  a 100 ms draw never drains it; the yields inside the draw refill normally.
- A block with no steps is not sent. Rates are integers (steps/s ÷ 2): with 10 ms blocks a
  rate word is `steps × 50`, exact.

## What stays, what changes

| area | burst mode (removed 2026-11-02) | block mode |
|---|---|---|
| planner (`run_lines`, `run_simple`, S-curve, jerk, lookahead) | unchanged | unchanged — it produces per-axis positions per 10 ms |
| emit | one burst per axis into an idle axis, rate pacing, deferrals, `BURST_MAX_S`, burst-end wake | one block per 10 ms with all axes; no deferral, no cap, no wake |
| level gate `axis_blocked` (was `burst_blocked`; E-stop, drives off, tripped switch) | per axis per burst | per axis per block: that axis' steps are dropped (position rebased), the others go |
| runaway guard (a delta no plan can produce) | refuse + stop | same |
| graceful stop | planner brakes | same: the brake is in the blocks |
| immediate stop | drop unsent steps; the burst in flight finishes | drop unsent steps; `fpga_block_flush()` (mode off/on) — to be measured; worst case the queue (≤ 50 ms) plays out |
| homing | bursts, counter preset between phases | blocks; the preset waits for the queue to drain (`home.phase` 2 waits for `fpga_block_queue() == 0 && !moving`) |
| `motion_idle()` | planner idle | planner idle **and** queue empty **and** nothing moving |
| position check at rest | after 3 heartbeats at rest | same |
| sim (`sim/fpga_sim.c`) | burst model | + a 256-deep block queue played at the block's rate, status bits 4+ch/12+ch, `0x36` depth; needs the watchdog |
| status / trace | `bursts`, `reversalsWaited` | + `blocks`, `queue` |
| main loop | burst-end wakes, `slice_for_frame`, `BURST_MS` | ticks at 10 ms wall cadence; `motion_feed_ahead(frame_ms)` before a frame; the wake machinery is removed |

## Files

- `hal/bus.h`, `bus_ioctl.c`, `bus_sim.c`: `bus_block(b, w[16])` (ioctl `0x4622` / sim).
- `hal/fpga.h`, `fpga.c`: `fpga_block_mode()`, `fpga_block()` (axes → channels, the word
  layout), `fpga_block_queue()`, `fpga_moving()` (block layout), `fpga_block_flush()`.
- `sim/fpga_sim.c`: the queue model. `tests/test_fpga.c`: encoding and model tests.
- `core/motion.c`: `emit_block()`, the queue-driven tick, homing drain wait, `motion_idle`.
- `core/settings.c`: `motion.queue_ms` (default 50). (`motion.block_mode` removed 2026-11-02.)
- `main.c`: wall-cadence ticks, `motion_feed_ahead` before a frame; wake code removed.
- `net/api.c`, `core/ctl_tick.c`: status fields, trace columns.
