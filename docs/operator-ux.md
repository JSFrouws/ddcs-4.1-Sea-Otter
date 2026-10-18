# Operator UX: functions, user stories, screens, navigation

_2026-10-17. The operator-facing design for the replacement app on the DDCS V4.1 panel
(1024×600 16bpp LCD, confirmed over SSH, no touch, 18 keys + MPG pendant). Builds on
[ui-hardware-constraints.md](ui-hardware-constraints.md) (what the panel can do),
[ui-requirements.md](ui-requirements.md) (requirement IDs), and
[workflow-and-resume.md](workflow-and-resume.md) (simulate / TryCut / run / resume).
Everything the panel can do is also exposed over Ethernet — see
[ethernet-api.md](ethernet-api.md). Visual mockup:
[../mockup/ui-proposal.html](../mockup/ui-proposal.html)._

## Product statement

A hobby CNC controller app for the DDCS V4.1 that is **quick to operate from the
panel keys and pendant**, **not overly protective**, and **fully controllable over
Ethernet through an extensive API**. The API covers every step of operating the
machine: powering and stopping the drives, homing the axes, jogging and moving,
probing, uploading and loading programs, starting / pausing / stopping a `.nc`
program, resuming from a line, overrides, spindle and coolant, offsets, tools, I/O,
and settings. The panel UI is just one client of the same command layer, so anything
the panel can do, a remote client can do too.

## Design principles

1. **One press for common things.** Start, Pause, Reset, Spindle, TryCut are their own
   keys. Any screen is at most two presses away (`Shift` + a bottom key jumps
   directly; `Cancel` always returns to Run).
2. **Block only what's actually unsafe, and only that.** A tripped limit blocks motion
   *into* that limit on *that* axis. Every other axis, and the way out, stay usable.
   No forced re-home, no lock-out screens. (§ Limit policy.)
3. **No confirmation dialogs for routine actions.** Exactly one deliberate confirmation
   exists: confirming the physical position before resuming mid-program, because a
   wrong assumption there crashes the tool.
4. **State is always visible.** Mode, machine state, drives, active WCS, tool, spindle,
   overrides and alarms are on screen at all times, and mirrored on the panel LEDs.
5. **Same words everywhere.** The silkscreen key names are used on screen, so the soft
   label above a key never contradicts what's printed on it.

## Operator functions (by task)

| Task | Functions |
|---|---|
| **Power-up & readiness** | Release E-stop; enable/disable drives; see E-stop, drives, homed state; home all or one axis; clear alarms |
| **Program handling** | Browse Local / USB / Network share; preview toolpath, line count, bounding box, run-time estimate; load; receive programs over the API from the CAM PC |
| **Work setup** | Jog (keys: step or continuous; pendant: wheel); set work zero per axis or all; go to work zero; Z touch-off with a plate; tool-length probe; edge / corner / centre finding; choose WCS G54–G59 |
| **Verify** | Simulate (desktop backplot, no motion); bounds/soft-limit check; TryCut dry run (spindle suppressed, Z clamped to a safety height); live position over the planned path |
| **Run** | Start; Pause (feed hold); Resume; Stop; feed / spindle / rapid overrides live from keys or pendant; progress, line, elapsed/remaining; M0/M1/M6 prompts |
| **Recover** | Resume from the last completed line or any chosen line/section; continue after a limit hit (jog out); after E-stop (re-enable drives, optional re-home); after power loss (work-done log) |
| **Manual work** | Spindle on/off/speed; coolant flood/mist; macros (go to safe Z, park, tool-change position, spindle warm-up) |
| **Tools & offsets** | Edit WCS table; set from current position; tool table (number, length, diameter, note) |
| **Diagnostics** | Live inputs (limits, home, probe), outputs OUT1–3, pendant inputs, spindle PWM, FPGA version, connected API clients, register trace (service mode) |
| **Remote** | Everything above over the Ethernet API; one controlling client at a time; panel can always take control back |

## User stories

Format: *As an operator, I want … so that …* — with the screen and keys it uses. IDs are
stable for later tickets.

### Power-up & homing
- **US-01** I want to switch the drives on with one action after releasing the E-stop, so the machine is ready without hunting through menus. *Home screen · `Shift+Pause` (Home) then `Start`; API `POST /machine/drives`.*
- **US-02** I want to home all axes with one press, in my configured order (Z first), so my machine coordinates are trustworthy. *Home screen · `Enter/Home`.*
- **US-03** I want to home a single axis, so I don't move axes I just set up. *Home screen · arrows to pick axis, `Select A`.*
- **US-04** I want to see at a glance which axes are homed, so I know whether soft limits are active. *Top bar chip + Home screen.*

### Programs
- **US-05** I want to pick a file from USB, local storage, or the network share with the arrow keys, so I don't need a PC at the machine. *Files screen · `Y+/Y−`, `X−/X+` to change source, `Start` to load.*
- **US-06** I want a toolpath preview, bounding box and run-time estimate *before* loading, so I catch a wrong file or wrong origin early. *Files screen preview pane.*
- **US-07** I want to send a program from my CAM PC straight to the controller and have it ready to run, so I skip USB sticks. *API `POST /programs` + `POST /program/load`.*
- **US-08** I want a parse error to show the line number and the unsupported code, so I can fix the post-processor. *Files / Run banner.*

### Work setup
- **US-09** I want to jog with the arrow keys in steps of 0.01 / 0.1 / 1 / 10 mm or continuously, so I can approach the stock quickly and then precisely. *Jog screen · arrows; `X−/X+` with `Shift` for coarse.*
- **US-10** I want to jog with the pendant wheel while holding the dead-man, axis and multiplier chosen on the pendant, so I can stand at the stock while doing it. *Any screen except Run-in-progress.*
- **US-11** I want to zero one axis (or all) of the active WCS from the current position in one press, so setting up a job is fast. *Jog screen · `Shift` + that axis's key; Offsets screen · `Zero`.*
- **US-12** I want to touch off Z with a probe plate, with the plate thickness remembered, so tool changes don't need a paper test. *Probe screen · pick "Z plate", `Start`.*
- **US-13** I want edge, corner and centre probing routines, so I can locate the stock without measuring by hand. *Probe screen.*
- **US-14** I want to go to work zero at a safe height with one key, so I can check my origin visually. *Jog screen · `Shift+Goto0`.*

### Verify
- **US-15** I want to simulate a program on screen without moving anything, so I can check the shape and the order of operations. *Run screen · `Start` while "Simulate" is armed (`TryCut` cycles Run → TryCut → Simulate).*
- **US-16** I want a dry run (TryCut) that keeps the spindle off and Z above a safety height, so I can watch the real machine trace the job in free air. *`TryCut` arms, `Start` runs.*
- **US-17** I want the live tool position drawn over the planned path, so I see drift or a wrong offset immediately. *Run screen toolpath.*

### Run
- **US-18** I want Start / Pause / Resume / Stop on dedicated keys that work from every screen, so I never have to navigate to stop or hold. *`Start`, `Pause`, `Reset`.*
- **US-19** I want to trim feed, spindle and rapid overrides from the pendant while cutting, so I can react to chatter without letting go of the machine. *Pendant: axis switch on OFF/5/6, dead-man held, multiplier selects which override (§ Pendant).*
- **US-20** I want progress (line, %, elapsed, remaining) always visible, so I can plan around the job. *Run screen.*
- **US-21** I want M6 tool changes to pause, show which tool to fit, and offer a tool-length probe, so manual tool changes are quick and correct. *Prompt overlay · `Start` continues.*

### Recover
- **US-22** I want the controller to remember the last completed line and modal state through a crash, E-stop or power loss, so I can continue a long job. *Work-done log (workflow-and-resume.md §3).*
- **US-23** I want to choose any line or section with the pendant wheel and see the reconstructed state (WCS, feed, spindle, tool, position), so I can restart after a broken tool. *Program view.*
- **US-24** I want the controller to move to the resume point at safe Z after one confirmation, so I don't jog there by eye. *Program view · `Select A` then `Start`.*
- **US-25** When a limit switch trips, I want to keep jogging the other axes and jog the tripped axis away from the switch, without re-homing, so a small mistake costs seconds, not minutes. *Jog screen (§ Limit policy).*
- **US-26** After an E-stop, I want to re-enable the drives and continue (re-homing offered, not forced), so I'm back to work quickly. *Home screen.*

### Manual & diagnostics
- **US-27** I want to switch spindle and coolant by hand for setup and testing. *`Spindle` key; I/O screen for coolant.*
- **US-28** I want a short macro list (safe Z, park, tool-change position, warm-up), since the panel has no keyboard for MDI. *Macros screen (`Mode`).*
- **US-29** I want to see every input (limits, home, probe, pendant) and toggle outputs, so I can test my wiring. *I/O screen.*
- **US-30** I want to see which remote client is connected and whether it holds control, so I'm never surprised by a remote command. *Top bar LAN chip + Ethernet LED; I/O screen.*

### Remote / automation
- **US-31** As an automation script, I want to power drives, home, load a program, probe Z, start it and get progress events, so the machine can run unattended steps. *ethernet-api.md "Typical job sequence".*
- **US-32** As a remote user, I want to watch the machine state and position live from a browser on the LAN, read-only, without taking control. *API WebSocket, observer role.*

## Screens

> **Superseded 2026-10-18** by [operator-procedures.md](operator-procedures.md): six
> screens (Run, Jog, Files, Work zero & probe, Tools, Settings), Mode = Run ↔ Jog,
> Page R = page rotate (as in the vendor app), Tab = next pane, per-screen F1-F6 soft
> keys, a state machine that locks all directional controls while a program runs, a
> digit-at-a-time number editor and Shift touch-off with an edge Ø. The tables below
> are kept for the record; the user stories above still apply.

Eleven screens plus three overlays. All 1024×600, flat dark-marine theme, white figures.

| # | Screen | Purpose | Reached by |
|---|---|---|---|
| 1 | **Run** | DRO (work + machine), toolpath with live tool, progress, current/next line, overrides, armed mode (Run / TryCut / Simulate) | Default · `Cancel` from anywhere · `Mode` cycle |
| 2 | **Program** | Toolpath + listing; pick a resume line/section; reconstructed state card | `View` on Run |
| 3 | **Jog** | Big DRO, step size, jog feed, per-axis limit chips, pendant state, zero/goto-zero | `Mode` cycle |
| 4 | **Macros** | Preset moves/commands instead of MDI typing | `Mode` cycle |
| 5 | **Files** | Local / USB / Share browser with preview and stats | `Shift+Start` · `Page R` cycle |
| 6 | **Home & Drives** | E-stop state, drives on/off, homed per axis, home all / single | `Shift+Pause` · `Page R` |
| 7 | **Probe** | Routines (Z plate, tool length, edge, corner, centre) and their parameters, last result | `Shift+Reset` · `Modify/Probe` (long-press) · `Page R` |
| 8 | **Offsets** | G54–G59 table, set from current, edit | `Shift+View` · `Page R` |
| 9 | **Tools** | Tool table | `Shift+TryCut` · `Page R` |
| 10 | **I/O & Diagnostics** | Inputs IN01–13 with assigned functions, OUT1–3, pendant bits, spindle PWM, FPGA version, API clients, trace | `Shift+Spindle` · `Page R` |
| 11 | **Settings** | Axes, limits & homing policy, spindle, I/O mapping, pendant, network & API, display | `Shift+Page R` · `Page R` |

Overlays (non-blocking unless noted): **alarm banner** (top of content, never a modal),
**prompt card** (M0/M1/M6, `Start` continues), **resume confirmation card** (the one
deliberate confirm).

## Navigation model

- `Mode` cycles the operating screens: **Run → Jog → Macros → Run**.
- `View` toggles within Run: **Run ↔ Program**.
- `Page R` cycles the setup pages: **Files → Home → Probe → Offsets → Tools → I/O → Settings → Run**.
- `Shift` latches (Shift LED on) for one key: **`Shift` + bottom key jumps straight to a setup page** (Start=Files, Pause=Home, Reset=Probe, View=Offsets, TryCut=Tools, Spindle=I/O, Page R=Settings).
- `Cancel` returns to Run from anywhere. `Tab` moves focus between panes, `Y+/Y−` moves in lists, `X−/X+` changes tab/value, `Modify` edits the focused value, `Enter` confirms, `Select A` picks.
- **On the Jog screen the arrow keys jog** (X−/X+, Y+/Y−, Z+ = Cancel, Z− = Enter), with `Shift` + axis key zeroing that axis and `Shift+Goto0` going to work zero. `Cancel`/`Enter` keep their Z jog meaning there; `Mode` leaves.
- `Start`, `Pause`, `Reset`, `Spindle` act globally from every screen (US-18).

## Panel LEDs (7 available, [ui-controls-map.md](ui-controls-map.md))

| LED | Meaning |
|---|---|
| Start | Program running (blinks while TryCut/simulate runs) |
| Pause | Feed hold |
| Reset | Alarm active (blinks for a limit, steady for E-stop) |
| Spindle | Spindle on |
| Shift | Shift latched |
| Ethernet | A remote client holds control (blink = observer connected) |
| USB Disk | USB stick mounted |

## Limit policy — not overly protective

Hard limits are wired to `IN01–IN13` ([connector-pinouts.md](connector-pinouts.md)); each
input is assigned in Settings to `X+ / X− / X± (shared) / Y… / home / probe`. On the real
garage machine this isn't a hypothetical: the saved config
([machine-settings.md](machine-settings.md)) shows X, Y and Z **each use one shared input
for +limit, −limit, *and* home** (IN02/IN03/IN04) — confirming §5 below is the common
case here, not an edge case.

**When a hard limit trips:**
1. Motion stops at once. A running program goes to **Hold** (coordinated motion can't
   continue with one axis frozen). Spindle keeps its state unless configured to stop.
2. Only **that direction of that axis** is blocked. Example: X+ trips → `X+` jog is
   refused; `X−`, `Y`, `Z`, `A` jog normally, from keys and pendant.
3. The alarm is a **banner**, not a modal: "X+ LIMIT — X+ blocked · jog X− to release ·
   Y Z A free". The Reset LED blinks.
4. Moving off the switch clears the alarm **automatically**. No Reset needed, no
   re-home forced. Positions stay valid because the FPGA counts the steps it generated;
   a "re-home recommended" hint appears only if the drives were disabled or a stall is
   possible.
5. **Shared limit inputs** (both ends of an axis on one input, common in hobby wiring):
   the direction can't be known, so that axis may jog **both ways at reduced jog speed**
   until the input releases. Other axes stay free.
6. The program can be continued from the hold (`Start`) once the limit is clear, or
   resumed from a chosen line.

**Soft limits** (only once homed): a jog stops at the soft limit of that axis only; a
program whose bounding box exceeds the envelope is flagged at load (REQ-A3), with a
one-press "run anyway" — the operator decides.

**E-stop** (pendant `0x3D` bit 9 or panel/external) is the only global stop: drives
disabled, everything blocked until released and drives re-enabled (US-26).

## Pendant mapping (corrected 2026-10-17)

> **Extended 2026-10-18** in [operator-procedures.md](operator-procedures.md) § Pendant:
> wheel without hold = move (cursor/focus), hold + wheel = change value / overrides / jog,
> quick press of hold = Enter. The running-overrides mapping below is unchanged.

From [mpg-pendant-io.md](mpg-pendant-io.md): the axis switch reports X / Y / Z / 4 on
`0x3D` bits 3–6, but **positions OFF, 5 and 6 have no wires** — the hardware cannot tell
them apart. The dead-man gates the axis and multiplier bits; the multiplier
(X1 / X10 / X100, bits 0–2) is still reported when the axis switch is on OFF/5/6 and the
dead-man is held. So "no axis + dead-man held" is a fourth, usable state, and the
multiplier picks the function. Without the dead-man, the wheel does nothing (the app
ignores `0x0E` deltas then).

| Axis switch | Dead-man | Multiplier | Idle / program loaded | While running |
|---|---|---|---|---|
| any | released | — | wheel ignored | wheel ignored |
| X / Y / Z / 4 | held | X1 / X10 / X100 | jog axis 0.001 / 0.01 / 0.1 mm per count | jog blocked (overrides via OFF/5/6) |
| **OFF / 5 / 6** | held | **X1** | **scrub G-code line by 1** | **Feed override %** (1% per detent) |
| **OFF / 5 / 6** | held | **X10** | **scrub by 10 lines** | **Spindle override %** |
| **OFF / 5 / 6** | held | **X100** | **jump section to section** (tool change, G0 retract, comment) | **Rapid override %** |

This keeps the user's intent: positions 5/6 (and OFF) become the app's function wheel —
G-code stepping when idle, feed/spindle speed control while milling — just selected by
the multiplier instead of by 5 vs 6.
