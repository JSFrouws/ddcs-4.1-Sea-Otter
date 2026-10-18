# Requirements for a replacement hobby-CNC UI on the DDCS V4.1

_2026-10-15. Scope: the user-facing application that will replace `ddcsv4.out`
on top of our `/dev/motion` driver (see [driver-comparison.md](driver-comparison.md)).
This is a requirements and design document — no application code yet. The
companion documents are [workflow-and-resume.md](workflow-and-resume.md)
(simulate/dry-run/run workflow + "resume from a selected line") and
[architecture.md](architecture.md) (how it's built, what's reused)._

## Research: what comparable software already does

| Product | Class | Relevant to us |
|---|---|---|
| **EdingCNC (USBCNC V5)** | Motion-controller vendor software, same market segment as the DDCS (Ethernet-connected controller box + PC/tablet UI) | Loads a file and fully parses it through the G-code interpreter up front so the toolpath backplot is shown before any motion (confirmed in their manual text); restart after a program-paused `M0` resumes at cycle start; canned drilling cycles `G81..G89`; a customizable screen designer ("E++") for building your own UI layout; a optional LED/traffic-light **StatusView** for at-a-glance machine state — closest analogue to what we're replacing, since it's the same "Ethernet box + thin client" shape as the DDCS |
| **LinuxCNC (AXIS/gmoccapy GUIs)** | Full PC-based CNC, open source | "Run from line" exists but is explicitly limited: it restores position only in a limited way, and the **operator must manually restart the spindle and re-establish state** (confirmed by multiple user reports) — direct evidence that resume is a hard problem even in mature software, and that "restore state automatically" is a real value-add, not a solved problem to skip |
| **PlanetCNC TNG** | Hobby/small-shop G-code sender + simple motion controller | Has "Start From Selected Line" specifically for recovering from a crash/broken tool/power loss. Vendor's own tutorial warns it can send the cutter across the workpiece if misused, and that **if the motors lost steps, the saved coordinates are no longer trustworthy** — this is the key safety lesson behind our resume design (§ in workflow-and-resume.md): never silently trust a remembered position, always let the operator confirm it |
| **Mach3 / Mach4 / UCCNC** | PC-based, the hobby/prosumer mainstream | Mach4 is valued for a complete scripting environment and a screen editor for OEM-style custom UIs; UCCNC is valued for being touchscreen-friendly and easy to re-skin. Both support jog-while-running via MDI. Take-away: **customizable screens and touch-friendly controls are expected**, not a luxury |
| **CNCjs / bCNC / Candle / Universal Gcode Sender** | Lightweight hobby G-code senders for GRBL-class controllers | All ship a 2D/3D toolpath visualizer and a simple DRO + jog pad as table stakes, and CNCjs runs as a **local web server with a browser UI** — the same shape we're proposing (see architecture.md) rather than a native desktop app |

Common thread across all of them: **parse-and-backplot before running**, **jog
pad + DRO always visible**, **toolpath visualization during the actual cut**,
and **some notion of restart-from-a-point**, universally hedged with warnings
that restart is only as good as the position data behind it.

## Requirements

Tagged MoSCoW-style: **M**ust / **S**hould / **C**ould / **W**on't (for this
phase). IDs are stable references for later design docs and tickets.

### A — Safety & machine state (not "extremely protective", but not silent either)

- **REQ-A1 (M)** A single always-visible machine-state indicator: `DISCONNECTED
  / IDLE / JOGGING / RUNNING / HOLD / ALARM`. No hidden state — this is the
  EdingCNC "StatusView" idea, built into the main screen instead of an add-on.
- **REQ-A2 (M)** A single, always-reachable **Stop** control that halts motion
  and spindle immediately, from any screen/tab, without confirmation dialogs
  in the way.
- **REQ-A3 (M)** Soft-limit awareness: if axis travel limits are known/entered,
  flag a job whose bounding box exceeds them at load, with a clear message —
  not a silent clamp — and a one-press "run anyway" (the operator decides).
  Jogging stops at the soft limit of that axis only.
- **REQ-A6 (M)** Per-axis, per-direction hard-limit handling: a tripped limit
  stops motion and blocks only motion *into* that limit on that axis. All other
  axes and the way off the switch stay usable; the alarm clears automatically
  once the switch releases; no forced re-home. Shared limit inputs allow both
  directions at reduced jog speed. Full policy: [operator-ux.md](operator-ux.md).
- **REQ-A7 (M) Controlled standstill - never lose steps** (user, 2026-10-18).
  Setting *Homing & limits › Controlled stop on Reset, E-stop, limit* (key
  `motion.graceful_stop`, **default on**). When on, every stop that is not a
  power-off - Reset, the pendant / external E-stop, a hard limit switch -
  brakes **along the programmed path** at the axes' configured **max
  deceleration** (`axis.*.accel`, S-curve with `axis.*.jerk`), so the steppers
  never lose a step and the position stays valid: no re-homing, no re-zeroing,
  resume from a line works directly. The overshoot is the braking distance
  v²/2a (≈ 0.67 mm at 1200 mm/min, 300 mm/s²) and **is the user's to allow
  for**: place limit switches that far before the hard stop. When off, motion
  halts after the burst already in the FPGA (≈ one 10-40 ms slice), which at
  speed can lose steps on a real machine. Switching the drives off (F3 Drives,
  API) is always immediate. Simulation stops are always immediate (nothing moves).
- **REQ-A8 (M) The E-stop stops motion, it does not remove power** (user,
  2026-10-18). The E-stop is for *unexpected or undesired movement*, not for
  fire: it stops motion (controlled, REQ-A7), the spindle and the outputs, but
  **the drives stay enabled** so holding torque and position are kept. Releasing
  it returns to IDLE - no Reset / drives-on step. To remove power, switch the
  machine off. (The FPGA has no confirmed drive-enable output anyway, see
  ethernet-api.md.)
- **REQ-A9 (M) Easy recovery after a limit** (user, 2026-10-18). After a limit
  stop the operator can at once jog **sideways** (other axes), **back** (the
  same axis away from the switch) or **up/down** (Z); only the tripped
  direction is blocked and the limit clears itself when the switch releases.
  An interrupted program is remembered (work done) and resumes from a line.
- **REQ-A4 (S)** Before any motion after an idle/paused period, show (not
  silently assume) the modal state about to be used: WCS, units, feed,
  spindle speed/direction, tool. This directly answers the LinuxCNC/PlanetCNC
  lesson above: **never resume state invisibly.**
- **REQ-A5 (M)** Explicitly **not** a goal: interlocked doors, light curtains,
  redundant E-stop chains, or any certified safety system. This is a hobby
  machine UI, not a CE-marked safety product — the user asked for "not
  extremely protective," and real safety-critical interlocks (if ever needed)
  belong in hardware, not this app. (**W**on't, by explicit request.)

### B — Connection & machine state

- **REQ-B1 (M)** Connect to either the real `/dev/motion` (on-device) or the
  mockup simulator (`mockup/`) through the same UI, selectable at startup —
  this is the `MotionBackend` abstraction in architecture.md.
- **REQ-B2 (S)** Live readout of raw FPGA trace activity (reusing
  `/dev/motion-trace`) in a collapsible diagnostics panel — useful for us
  during bring-up, harmless to hide for normal use.

### C — File & G-code management

- **REQ-C1 (M)** Browse and open `.nc`/`.tap`/`.gcode` files from: the local
  filesystem, the `CNCDISK` share, and a USB stick path — mirroring
  `ddcsv4.out`'s own file sources (`/local`, `/udisk`, `/share`, see
  [app-analysis.md](app-analysis.md)).
- **REQ-C2 (M)** Parse the whole file on load (like EdingCNC) and show parse
  errors/unsupported codes *before* anything is run, with the offending line
  numbers.
- **REQ-C3 (S)** Recently-opened list and a basic in-app text viewer with
  line numbers (not a full G-code editor — editing is explicitly out of scope
  for v1; use a real text editor and reopen).
- **REQ-C4 (C)** Syntax highlighting of G/M-codes, comments, and the
  currently-executing line during a run.

### D — Simulation (desktop backplot, no machine motion)

- **REQ-D1 (M)** Full toolpath preview computed purely from the parsed file:
  rapids vs. feeds visually distinct, current work offset applied, estimated
  run time from feedrates.
- **REQ-D2 (M)** Bounding-box / soft-limit check against the configured
  machine envelope (REQ-A3), run automatically on load.
- **REQ-D3 (S)** Scrub the simulated toolpath by line number or by dragging a
  playhead, independent of the real machine — this is the same code path
  used for "preview where line N resumes" (workflow-and-resume.md).
- **REQ-D4 (C)** Basic 3D view (extruded stock removal) — explicitly a
  "could", since every comparable product (CIMCO, EdgeCAM) treats this as a
  premium feature, not a baseline one; a 2D plan + side view covers a router/
  mill hobby job adequately for v1.

- **REQ-D5 (M) Simulated coordinates are shown as such** (user, 2026-10-18):
  while *Simulate* runs, the DRO values are **orange** - they are fictitious;
  when it ends (finished, Reset, E-stop) the real position is restored and the
  DRO turns white again.

### E — Dry run ("TryCut": on the real machine, no cutting)

- **REQ-E1 (M)** Run the full program on the real machine with: spindle
  disabled (or forced to a safe no-contact state) and, where practical, Z
  lifted by a configurable safety offset, so the operator can watch the
  actual machine trace the job in free air.
- **REQ-E1a (M) TryCut keeps Z fixed** (user, 2026-10-18): TryCut moves Z
  once, straight to the **clearance level** (work Z zero + *Safe Z*) and then
  never changes Z - every program move is flown at that height, XY only. On a
  resume the approach also ends at that height.
- **REQ-E2 (M)** Live overlay of the *actual* machine position onto the
  simulated toolpath from §D while the dry run plays, so drift between
  "planned" and "actual" is visible immediately.
- **REQ-E3 (S)** Adjustable dry-run speed (e.g., capped feed override) so a
  large job can be watched at a sane pace rather than full speed.
- **REQ-E4 (S)** Abort-and-rewind: stopping a dry run should not require
  re-homing — it's not cutting anything, so it should be resumable or
  restartable immediately.

### F — Live run & visualization

- **REQ-F1 (M)** The same toolpath view as simulation/dry-run, now driven by
  live position feedback (via `MOTION_IOC_READ`/the block-latch sequence),
  with a clear marker for "where the tool actually is right now."
- **REQ-F2 (M)** Progress: current line number / total lines, elapsed time,
  estimated remaining time, visible at all times during a run.
- **REQ-F3 (M)** Feed and spindle override sliders, live-adjustable during a
  run (standard across every product surveyed above).
- **REQ-F3a (M) Overrides 0-300 %** (user, 2026-10-18): feed and spindle
  overrides go up to **300 %**; the pendant changes them in **2 % steps** (X1
  feed, X10 spindle; X100 = feed in 10 % steps). The bars share one scale:
  **full width = 200 %**, the bar stops there and the number shows the rest
  (amber above 200 %). Rapid follows the feed down and tops out at 100 %, so its
  bar is **half width** - its 100 % lines up with the 100 % point of the bars above.
- **REQ-F5 (M) CPU saver view** (user, 2026-10-18): the toolpath view's F4
  cycle is *2D → ISO wireframe → CPU saver*. CPU saver draws no toolpath and no
  tool marker and redraws the screen at most once a second while moving (keys
  still answer at once), so motion gets the CPU. The screen stays on. After an
  **E-stop or limit stop** it switches itself to the **isometric view and stays
  there** - the operator needs to see where the tool is. The start-up view is a
  setting (*Display › Default toolpath view*).
- **REQ-F4 (S)** Pause/Resume without losing modal state (the normal case —
  no crash involved), distinct from the recovery flow in §G.

### G — Resume / recovery ("always remember the work done")

See [workflow-and-resume.md](workflow-and-resume.md) for the full design;
requirements only here.

- **REQ-G1 (M)** Every run persists a durable, append-only log of which line
  last completed and the full modal state at that point (WCS, units, feed,
  spindle, tool, coolant) — the "work-done memory." Survives an app restart
  or power loss (it's written to disk as it happens, not just held in RAM).
- **REQ-G2 (M)** The operator can select **any** line (not just the last
  completed one) in the loaded program and ask "what would the machine state
  be at this point" — computed by replaying the interpreter, not by trusting
  stale machine state.
- **REQ-G3 (M)** Resuming from a selected line always shows the reconstructed
  state (WCS/feed/spindle/tool/position) and **requires explicit operator
  confirmation** of the physical XYZ position before any motion — directly
  addressing the PlanetCNC "lost steps make saved coordinates worthless"
  lesson. This is a confirmation step, not a safety interlock (REQ-A5) —
  it's one click, not a gate.
- **REQ-G4 (S)** Snap suggested resume points to block boundaries that start
  with a rapid to a new XY (not mid-arc, not mid-pocket) — the LinuxCNC forum
  guidance "pick a point with a G0 to a new XY and then Z height."
- **REQ-G5 (C)** Visual diff on the toolpath view between "already cut" (from
  the work-done log) and "remaining" when a job is reloaded after an
  interruption.

### H — Jogging & manual control

- **REQ-H1 (M)** On-screen jog pad (X/Y/Z, step sizes 0.01/0.1/1/10 mm and
  continuous) usable with mouse/touch.
- **REQ-H2 (M)** Live MPG/handwheel support via `MOTION_IOC_MPG_COUNT` —
  the physical handwheel should work identically through the new UI as it
  did through `ddcsv4.out`, since the driver ABI is unchanged.
- **REQ-H3 (S)** "Jog to resume position" helper: one button that rapids
  (at a safe Z first) to the XY the interpreter reconstructed for a chosen
  resume line (REQ-G2), so the operator isn't jogging by eye.

### I — Work coordinates, tool table, offsets

- **REQ-I1 (M)** G54–G59 work offset table: view, edit, "set from current
  position" (touch-off).
- **REQ-I2 (S)** Simple tool table (number, length offset, diameter/comment)
  — no automatic tool changer support assumed, matching the real machine.
- **REQ-I3 (C)** Probing routine (single-axis touch-off) if/when a probe
  input is wired — depends on hardware not yet confirmed present.

### J — I/O, spindle, coolant

- **REQ-J1 (M)** Manual spindle on/off/speed and coolant on/off controls,
  independent of a running program (for setup/testing), using the existing
  `MOTION_IOC_SPINDLE` ioctl.
- **REQ-J2 (S)** Raw digital I/O panel (read AUX inputs, toggle AUX outputs)
  for commissioning — the FPGA register file already exposes this via
  `MOTION_IOC_WRITE`/`READ`; just needs a UI.

### K — Diagnostics & tracing

- **REQ-K1 (S)** A built-in trace viewer reusing `/dev/motion-trace` as-is:
  live register read/write log, same semantics as the `mtrace` CLI
  tool, for when something behaves unexpectedly — this turns our existing
  reverse-engineering instrument into a permanent service-mode screen instead
  of a one-off tool.
- **REQ-K2 (C)** Export a trace capture to file directly from the UI (already
  possible with `mtrace dump`; a UI button is a convenience, not new
  capability).

### L — UI/UX & extensibility

- **REQ-L1 (M)** Fully operable from the 18 physical panel keys and the MPG
  pendant — the panel is **not** a touchscreen (corrected 2026-10-16, see
  [ui-hardware-constraints.md](ui-hardware-constraints.md)). Every screen within
  two key presses; soft labels above the silkscreen F1–F7 keys.
- **REQ-L2 (M)** Not "extremely protective": no confirmation dialogs for
  routine actions (opening a file, jogging, starting a simulation); reserve
  confirmation for the few places that move real metal unexpectedly (starting
  a run, resuming mid-program — REQ-G3).
- **REQ-L3 (C)** Screen/layout customization in the spirit of EdingCNC's E++
  and Mach4's screen editor — explicitly deferred past v1; the fixed layout
  proposed in this round should cover the common case first.

### N — Remote control over Ethernet

- **REQ-N1 (M)** The application is **fully controllable over Ethernet through an
  extensive API**: power/stop the drives, home the axes, jog and move, probe,
  upload/load a program, start/pause/stop a `.nc` program, resume from a line,
  overrides, spindle, coolant, offsets, tools, I/O and settings. Spec:
  [ethernet-api.md](ethernet-api.md).
- **REQ-N2 (M)** Parity: the panel UI uses the same command layer as the API, so
  nothing is panel-only.
- **REQ-N3 (M)** One control lease (panel or one remote client); unlimited read-only
  observers; the panel can always take control back; E-stop and Stop always win.
- **REQ-N4 (M)** Live event stream (state, position, line, alarms, prompts, I/O).
- **REQ-N5 (S)** Token auth, API on/off switch, LAN-only. Raw FPGA register writes only
  in service mode.

### O — Panel conveniences (added 2026-10-18)

- **REQ-O1 (M) Macros page** (Page R: Run → Files → Work → Tools → **Macros** →
  Settings): Safe Z (up only), Go to work zero (Z up, then XY), Park and Tool
  change position (machine coordinates from *Settings › Macros*, need a homed
  machine; Z up first, then XY, then Z to its height), Spindle warm-up (rpm and
  time are settings, Reset stops it), Zero all (X Y Z of the active WCS = here).
  Also over the API: `POST /move/macro`.
- **REQ-O2 (M) Settings that don't apply are greyed out** and can't be edited,
  with the reason shown: API port / remote start / agent messages when the API
  is off; start countdown when remote START is off; pendant settings with no
  pendant; E-stop level with no E-stop input; soft-limit values with soft limits
  off; warm-up time with warm-up speed 0.

### M — Explicit non-goals (v1)

- **Won't:** G-code editing in-app (open in a real editor, reopen to reload).
- **Won't:** Automatic tool changer sequencing.
- **Won't:** Certified safety interlocks (REQ-A5).
- **Won't:** Multi-job queueing/scheduling — one job loaded and run at a time.
- **Won't:** Cloud accounts, telemetry, or any networked service beyond the
  machine's own LAN (the Ethernet API in §N is LAN-only).
