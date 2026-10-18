# Proposed architecture: replacing `ddcsv4.out`'s user-facing side

_2026-10-15. Satisfies [ui-requirements.md](ui-requirements.md) REQ-B1 and
implements the stages in [workflow-and-resume.md](workflow-and-resume.md).
This is a proposal to agree on, not yet built — see the note at the end._

## 1. What's reused unchanged, what's rewritten, and why

| Layer | Decision | Reasoning |
|---|---|---|
| `driver/motiondev.c`, `motiondev_lld.c`, `motiondev_hw.h` | **Reuse unchanged** | This is the hardware ABI layer, already a confirmed, working reimplementation of the vendor driver (see [driver-comparison.md](driver-comparison.md)). Nothing about a new UI changes what the FPGA bus needs. |
| `/dev/motion-trace` + `mtrace` | **Reuse unchanged** | Already does exactly what REQ-K1 needs; becomes a permanent diagnostics screen instead of a standalone tool, not a rewrite. |
| `mockup/` (`motion_sim.py`, `server.py`, `fake_motion_client.py`) | **Reuse as the Sim backend**, with a thin protocol adapter if the real app ends up in a different language | It already speaks the exact ioctl-shaped API (`write/read/block/beep/mpg_count/led/spindle/key`) this architecture needs for development without hardware. |
| `ddcsv4.out`'s UI and G-code handling | **Rewrite (there's nothing to reuse)** | Closed binary, no source. Replacing it is the point of this effort. The upstream Pandora-CNC `pdui` attempt (docs/upstream-research.md) never got far enough to reuse either — it's SDL drawing primitives, no G-code or motion logic. |
| G-code interpreter, job state tracking, resume log | **New** | Nothing in this project or upstream Pandora-CNC has this; see workflow-and-resume.md. |

## 2. Component diagram

```
                     +-----------------------------------+
                     |     Browser UI (REQ-L1 touch)      |
                     |  file picker / program / jog /     |
                     |  toolpath canvas / DRO / overrides |
                     +------------------+------------------+
                                        | WebSocket (state push)
                                        | REST (commands)
                     +------------------v------------------+
                     |           App backend (new)          |
                     |                                       |
                     |  +-------------+   +----------------+ |
                     |  |  G-code     |   |  Job state /   | |
                     |  |  interpreter|-->|  resume log    | |
                     |  |  (simulate  |   |  (SQLite)      | |
                     |  |   mode)     |   +----------------+ |
                     |  +------+------+                      |
                     |         | motion command stream        |
                     |  +------v------+                       |
                     |  | Block       |  (word-order/scale    |
                     |  | encoder     |   isolated here, see  |
                     |  +------+------+   §3 below)           |
                     |         |                               |
                     |  +------v-----------------------+       |
                     |  |     MotionBackend interface   |       |
                     |  +--------------+----------------+       |
                     +-----------------|------------------------+
                                        |
                  +---------------------+----------------------+
                  |                                             |
        +---------v----------+                       +---------v----------+
        | SimMotionBackend    |                       | RealMotionBackend   |
        | -> mockup/server.py |                       | -> /dev/motion      |
        |    (dev machine)    |                       |    /dev/motion-trace|
        +---------------------+                       | (on the DDCS board) |
                                                        +----------------------+
```

### Why a browser UI talking to a local backend (not a native desktop app)

Matches CNCjs's shape (ui-requirements.md research) and solves two real
constraints at once: the DDCS is itself a Linux box already reachable over
Ethernet (per [network.md](network.md)), so the backend can run **on the
controller** and be driven from any browser on the shop network (a tablet at
the machine, a PC at the desk) without installing anything per-client; and
during development, the same backend runs on a dev PC against
`mockup/server.py` with zero code changes to the UI.

### `MotionBackend` interface

One small interface both backends implement, mirroring the 9 ioctls 1:1
(`write/read/block/beep/mpgCount/led/spindle/pollKey`) plus a trace
read/subscribe for REQ-K1. This is deliberately the same shape as
`fake_motion_client.py`'s `FakeMotion` class already — `RealMotionBackend` is
the only genuinely new piece here, and it's a thin layer: open `/dev/motion`
and `/dev/motion-trace`, issue the same ioctls `motion_ioctl_impl` in
`driver/motiondev.c` already implements, nothing more.

### Block encoder — isolated on purpose

`MOTION_IOC_BLOCK`'s 18-word payload format is still one of the open
"Unknowns" ([README.md](../README.md), confirmed straight-through but
unverified word meaning in [driver-comparison.md](driver-comparison.md) §7).
The interpreter should never produce raw block words directly — it produces
an abstract motion command (target position, feed, move type), and a single
`block encoder` module turns that into the 18 words. When an `mtrace`
capture finally nails down the real word layout, only this one module
changes; the interpreter, UI, job log, and both backends are untouched. The
mockup's own block interpretation (`mockup/motion_sim.py`'s docstring) is
explicitly labeled as a placeholder for exactly this reason.

## 3. G-code interpreter: reuse or write?

Recommendation: **write a small one**, not adopt a full external interpreter
(e.g. a LinuxCNC/`rs274` style canonical interpreter) — matching the user's
"rewrite is fine" allowance. Reasoning:

- The DDCS/vendor app's supported G-code subset is modest (see the `.nc`/
  `.rc` samples already in `docs/raw/sysdisk-2026-10-15/`: linear/rapid
  moves, basic canned cycles, work offsets, M-codes for spindle/coolant) —
  nowhere near the full RS274NGC spec LinuxCNC implements.
- A full external interpreter brings its own canonical-motion abstraction
  that would need adapting to our block encoder anyway; a small
  purpose-built one can emit exactly the motion-command shape §2 needs.
- It needs to run in two modes from the same code (simulate: update state,
  don't move; execute: update state, also enqueue motion) — easiest to
  guarantee by construction in a small interpreter than by wrapping a larger
  one not designed for that split.

Scope for v1: `G0/G1` linear, `G2/G3` arcs, `G17-19` planes, `G20/21` units,
`G54-59` WCS, `G90/91` abs/incr, `G90.1/G91.1` (if arcs need it), `G81-89`
canned cycles (matches what EdingCNC's own doc index calls out, and what the
captured `.rc`/`.nc` samples use — e.g. `probe-*.nc`, `home_*.nc`), `M3/M4/M5`
spindle, `M7/M8/M9` coolant, `M0/M1` pause, `M30` end, `M6` tool change
(manual pause + prompt, no ATC per REQ-M). Unsupported codes are a parse-time
error (REQ-C2), not a silent skip.

## 4. Job state & resume log

SQLite, one file, local to the backend (on the DDCS board or the dev
machine). Two tables, matching workflow-and-resume.md §3:

```sql
-- recomputed fresh every time the file is (re)loaded; keyed to content hash
CREATE TABLE sim_state (
  file_hash   TEXT, line_no INTEGER, modal_state_json TEXT, pos_json TEXT,
  PRIMARY KEY (file_hash, line_no)
);

-- append-only, one row per executed block, survives app/power restarts
CREATE TABLE run_log (
  run_id TEXT, ts REAL, file_hash TEXT, line_no INTEGER,
  modal_state_json TEXT, pos_json TEXT
);
```

## 5. Suggested stack (not load-bearing for the UI discussion)

- Backend: Node.js (easy WebSocket + SQLite + runs fine on the board's
  embedded Linux) or Python (consistent with `mockup/`, also fine on-board).
  Either is a detail to settle once the UI direction (next step) is agreed;
  it doesn't change anything in §1-4.
- Frontend: plain web (HTML/CSS/JS + `<canvas>` for the toolpath view), no
  heavy framework needed for the screen count in ui-requirements.md — keeps
  REQ-L1 (touch) and running well on whatever's driving the panel display.

## Status

This is a **design proposal**, matching the user's instruction to agree on
the UI first. Nothing in §2-5 is implemented yet. The next artifact is an
HTML mockup of the proposed screen layout, to agree on before any of this is
built for real.
