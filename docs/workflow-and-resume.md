# Workflow: select → simulate → dry-run (TryCut) → run, and resuming from a line

_2026-10-15. Companion to [ui-requirements.md](ui-requirements.md) (the
requirements this satisfies, REQ-D/E/F/G) and [architecture.md](architecture.md)
(how it's built). This document is the behavioural design — what happens, in
what order, and why — not the implementation._

## 1. The four stages, and why they're separate steps

```
  [ Select file ]
        |
        v
  [ 1. Parse + Simulate ]  <-- desktop only, no machine motion, instant
        |                      (REQ-D1..D4)
        v
  [ 2. Dry run ("TryCut") ] <-- real machine moves, spindle off / Z lifted
        |                      (REQ-E1..E4)
        v
  [ 3. Live run ]           <-- real cut, live overlay + overrides
        |                      (REQ-F1..F4)
        v
  [ Done / Interrupted ] -->[ 4. Resume from a line ]--> back to stage 3
                                 (REQ-G1..G5)
```

Each stage reuses the **same G-code interpreter and the same toolpath
renderer** — only what receives the computed motion changes (nothing, the
real machine at reduced risk, or the real machine for real). This is why the
architecture puts the interpreter and the `MotionBackend` behind a clean
interface (architecture.md §2): stage 1→2→3 is a matter of swapping which
backend consumes the same stream of motion commands, not three different
code paths that can drift apart.

### Stage 1 — Parse + Simulate (desktop, instant, no risk)

1. Load the file, run it fully through the G-code interpreter in
   **simulate mode**: every line updates an in-memory machine-state snapshot
   (position, WCS, units, feed, spindle, tool, modal G/M groups) but issues
   no motion.
2. Record one state snapshot **per line** (not just per motion block) — this
   per-line state table is exactly what stage 4 (resume) replays later, so
   it's computed once, here, and reused everywhere else.
3. Render the resulting toolpath (rapids vs. feed moves distinguished),
   compute the bounding box, compare against configured soft limits
   (REQ-A3/D2), and estimate run time from feed + rapid rates.
4. Report parse errors / unsupported codes with line numbers, before the
   operator can proceed to stage 2.

This stage never touches `/dev/motion`. It's pure computation and can run
even with no machine connected at all (useful for program-checking a job
file ahead of a shop session).

### Stage 2 — Dry run / TryCut (real machine, non-cutting)

1. Same interpreted motion stream as stage 1, now sent to the real
   `MotionBackend`, with two safety transforms applied before anything is
   sent to the FPGA:
   - Spindle commands are suppressed (or clamped to an explicit "off" —
     configurable, since some shops prefer to still hear the spindle ramp
     for timing checks).
   - Every Z-down move is clamped to a configurable **safety Z** (e.g.
     "never go below 5mm above the stock"), unless the operator explicitly
     disables the clamp for a specific careful dry run.
2. The toolpath view from stage 1 is shown live, with a second marker for
   actual machine position (read back continuously), so planned-vs-actual
   drift is visible as it happens (REQ-E2).
3. Dry run can be stopped and immediately restarted from the top at any
   time — there is no "work done" to protect, because nothing was cut
   (REQ-E4). No resume logic is needed here.

### Stage 3 — Live run

1. Identical motion stream, now sent unmodified to the real `MotionBackend`.
2. Before the first block is sent, the work-done log (§3) is opened for this
   job and a new run entry is started.
3. After **every** completed block, the current line number + full modal
   state + reported machine position is appended to that log (REQ-G1). This
   append is cheap (a few fields) and happens on the normal execution path,
   not as an afterthought — "always remember the work done" means this
   writes every block, not just on pause/stop.
4. Feed/spindle overrides (REQ-F3) apply here and only here — they must
   never silently apply during stage 2's TryCut in a way that could turn a
   "no contact" dry run into a real cut (e.g. a feed override does nothing
   dangerous if the spindle is off and Z is clamped, but an override that
   defeats the Z clamp should be its own explicit control, not reachable
   from the override slider).

### Stage 4 — Resume from a selected line

Triggered either by an interruption (E-stop, power loss, operator Stop) or
deliberately (operator wants to re-run from partway through, e.g. after
replacing a broken tool). Same UI path either way — there is no separate
"crash recovery mode," just "run from here," which keeps the mental model
small per REQ-L2 (not overly protective, one flow to learn).

1. Operator opens the job (same file — matched by content hash + size
   against the work-done log, see §3.3) and selects a line in the G-code
   view, or clicks "resume where it stopped" which pre-selects the last
   logged line.
2. The UI looks up (or recomputes, see §3.2) the **per-line state snapshot**
   from stage 1's table for that line — WCS, units, feed, spindle
   speed/direction, tool, and the XYZ position the interpreter expects at
   the start of that line.
3. **Snap to a safe boundary (REQ-G4):** if the selected line sits mid-arc
   or mid-pocket, the UI suggests the nearest preceding line that starts a
   rapid move to a new XY, and lets the operator confirm or pick a different
   line anyway.
4. The reconstructed state is displayed plainly — not hidden behind a
   confirmation dialog's fine print — e.g.:
   ```
   Resuming at line 842 (of 3,110)
     WCS       G54            Units    mm
     Feed      800 mm/min     Spindle  12,000 RPM CW
     Tool      T3             Coolant  Flood ON
     Position  X 142.300  Y 88.400  Z 5.000  (rapid height)
   ```
5. **Operator confirms the physical position** (REQ-G3): the UI offers
   "Jog to this position" (rapids at safe Z, then to XY, then down to the
   resume Z — REQ-H3) as the recommended path, but never moves there
   automatically without that explicit click. This is the one unavoidable
   "are you sure" in the whole app, because it's the one place a wrong
   assumption (lost steps, manually bumped machine) turns into a crash —
   exactly the PlanetCNC and LinuxCNC lesson from the research in
   ui-requirements.md.
6. On confirmation, the backend replays the **non-motion** modal setup for
   that state (spindle on/dir/speed, coolant, work offset select, units,
   feed rate) as real commands — then resumes streaming motion blocks from
   the selected line, identically to stage 3.

## 2. Visualizing while it runs

One toolpath canvas, shared by stages 1–4, layered:

1. **Base layer** (always, from stage 1): full planned path, rapids dashed,
   feed moves solid, colored by the program's own tool/feed changes if
   easy to derive, otherwise one neutral color.
2. **Work-done layer** (stages 3–4, from the log in §3): the already-executed
   portion of the path drawn in a visibly different color (e.g. dimmed),
   so reopening an interrupted job immediately shows what's left (REQ-G5).
3. **Live marker** (stages 2–3): current actual machine position, updated
   from read-back, drawn as a distinct dot/crosshair so it's never confused
   with the planned-path line it's following.
4. **Resume marker** (stage 4 only, while choosing a line): a highlighted
   point + a short state tooltip (the block from step 4 above), separate
   from the live marker so "where I'm about to resume" and "where the
   machine actually is right now" are never the same visual element.

## 3. "Always remember the work done" — the state-tracking strategy

### 3.1 Two different kinds of state, computed differently

- **Simulated per-line state** (stage 1 output): deterministic, recomputed
  any time from the G-code text alone. This answers "what *should* the
  state be at line N" and is what powers resume's state reconstruction
  (§1 Stage 4, step 2) and the scrubber (REQ-D3).
- **Observed work-done log** (stage 3 output): a plain append-only record of
  what actually happened — *not* recomputed, because it's the only source
  of truth for "how far did we really get" when a run is interrupted
  unexpectedly (power loss, E-stop) and the app itself may have restarted.

Keeping these separate matters: the simulated table says what *will* happen
if you resume at line N; the log says what *did* happen last time. Resume
uses the simulated table to show the operator what to expect, and the log
only to pick a sensible default line to offer first.

### 3.2 Storage

- One record per job file: keyed by `(file path, file size, content hash,
  mtime)` so a renamed, moved, or edited file is never silently matched to
  a stale log (if the file changed, the old per-line state table is invalid
  and must be recomputed before resume can be offered).
- One append-only run log per execution attempt of that job: a simple
  local store (SQLite table, or even a line-delimited JSON file — no need
  for a server-side database for a single-machine shop tool) with rows
  `(run_id, ts, line_no, modal_state_json, machine_pos_json)`.
- Written synchronously enough that a hard power loss loses at most the
  last in-flight block, not the whole run — this is the literal meaning of
  "always remember," so it has to survive the kind of event that makes
  resume necessary in the first place.

### 3.3 Staleness and trust

- If the job file's hash doesn't match what the log was recorded against,
  the UI refuses to offer "resume from log" and falls back to "pick a line
  and recompute" (§1 Stage 4) — recomputed state from the *current* file is
  always available even with no prior log at all.
- The log is a convenience for finding *where* to resume (a good default
  line), never the sole authority for *what state to apply* — that always
  comes from replaying the interpreter (deterministic, §3.1), so an edited
  comment or reordered line between runs can't silently feed stale modal
  state into a resume.
