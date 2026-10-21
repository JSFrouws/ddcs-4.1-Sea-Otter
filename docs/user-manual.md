# Sea Otter — DDCS V4.1 user manual

For the person at the machine. Sea Otter is the replacement controller software for the
DDCS V4.1 (`app/`). This manual covers daily use. Design background is in
[operator-procedures.md](operator-procedures.md), the network API in
[api-reference.md](api-reference.md).

> **Not yet tested on a machine with drives connected (2026-10-21).** Tested on a real
> controller with no drives or motors attached: step-exact positions, real CAM programs up
> to 64 000 lines, limit inputs and controlled stops through the real input path. Do the
> first real tests with the motors unpowered or able to move freely, with a hard-wired
> E-stop that cuts the drives. No warranty, no liability: see the README.

---

## 0. Installing

From a USB stick, no PC tools needed on the controller: [usb-install.md](usb-install.md).
In short: copy the `ddcsv4` install folder to a FAT32 stick, set the IP address in
`network.conf`, plug it in, switch on, then take the stick out. The browser panel and the
API answer at `http://<that IP>:8080/`. To go back to the vendor software, see the same page.
Remote access with a password: [ssh-setup.md](ssh-setup.md).

## 1. The screen

```
┌ mode │ screen title · program · line ───────────── WCS · Homed · state ┐
│ banner (only when something needs you: E-STOP, LIMIT, DRIVES OFF, countdown) │
│                                                          │ TAB   │
│                 screen content                           │ SHIFT │
│                                                          │ MODE  │
├ F1 ─┬ F2 ─┬ F3 ─┬ F4 ─┬ F5 ─┬ F6 ─┬ F7 ─┬ build ───────────────────────┤
```

- **Top right**: active work offset (G54…), `Homed ✓/—`, and the machine **state**
  (READY, RUN, HOLD, E-STOP, LIMIT, DRIVES OFF, SIMULATE…).
- **Soft keys F1–F7** along the bottom: their labels change per screen. **F7 is always
  Page R** (next screen); **Shift then F7** goes back.
- **Right strip**: what **Tab**, **Shift** and **Mode** do right now.
- **Toasts** (white box near the bottom): the result of what you just did, or why a key
  was refused. Nothing is refused silently.

## 2. Keys

| Key | Does |
|---|---|
| **Mode** | Run ↔ Jog. Jog is locked while a program runs or holds. |
| **F7 Page R** | Run → Files → Work → Tools → Macros → Settings → Run |
| **Tab** | Next pane on the screen (e.g. Toolpath ↔ Program) |
| **Shift** | ALT layer for the next key (lit while active): touch-off, Page R back, … |
| **Y± / X±** | Move in lists and tables; jog in Jog |
| **Enter / Select / Modify** | Open / accept / edit the focused item |
| **Cancel** | Back / discard |
| **F1 Start** | Start or resume the program |
| **F2 Pause** | Feed hold on **every** screen while something moves |
| **F3 Reset** | Stop on **every** screen. Also switches the drives on after DRIVES OFF |

## 3. Daily procedure

### 3.1 Power up
1. Switch on. If the E-stop is pressed, release it. The banner tells you what's needed.
2. **Mode** → Jog. **F1 Home** if you want soft limits (not required). Reset stops homing.

### 3.2 Load a program
1. **Page R** → Files. **F3** switches Local / USB. **Y±** picks a file.
2. **F1 Load** (or Select). The whole file is checked first; an unsupported code is
   reported with its line number and the file is not loaded.
   Supported: G0–G3 (also in the G17/G18/G19 planes), G4, G10 L2/L20, G20/G21, G28, G53,
   G54–G59, drilling cycles G73/G81/G82/G83/G85/G89 with G80 and G98/G99, G90/G91, G94,
   M0/M1, M3/M4/M5, M6 (pause for a tool change), M7/M8/M9, M10/M11, M30. G40/G49/G61/G64
   are accepted. G43 is accepted but ignored (no tool length compensation). G41/G42
   (cutter compensation) and G84 (tapping) are refused.

### 3.3 Zero the workpiece
1. Jog to the part's edge (Jog: arrow keys; **F4 Step** picks 0.01 / 0.1 / 1 / 10 mm or
   continuous).
2. **Shift + the axis key towards the part** sets that axis: with an edge Ø set (Jog setup,
   or Tools › F3) it accounts for the radius (X+ edge → X = −Ø/2). Press the same key again
   to set 0 instead.
3. Z: touch the top, **Shift + Z−** → Z = 0.
4. Check with **Goto 0** (Jog F2): Z up to safe height, then XY to 0.
5. Several parts: **Jog F5 WCS** picks G55, G56…; on **Work** mark them **F4 In job** and
   Start runs the program on each one in turn.

### 3.4 Check, then run
**F5 TryCut** on Run cycles what Start will do:

| Armed | What happens |
|---|---|
| **Run** | The real job |
| **TryCut** | Spindle off; Z goes once to the **clearance level** (work Z0 + Safe Z) and stays there. The machine flies the whole path in the air |
| **Simulate** | Nothing moves. The position readout turns **orange**: those coordinates are fictitious. Afterwards the real position is shown again. M0/M1/M6 pauses still stop it, just like a real run |

**F1 Start**. A program started over the network counts down on the panel first
(default 5 s); **Reset cancels**.

### 3.5 During the run
- **Overrides**: pendant switch OFF + hold the dead-man + turn the wheel:
  **X1 = feed**, **X10 = spindle** (2 % per click), **X100 = feed in 10 % steps**.
  Feed and spindle go up to **300 %**. The bars stop at 200 % and the number shows the rest
  in amber. **Rapid follows the feed down** and never goes above 100 %. Its bar is half
  width, so 100 % lines up on all three bars.
- **The speed is capped** at what the controller can actually deliver (960 mm/min per axis
  at 2000 steps/mm). A higher F or override runs at the cap.
- **F4 View** cycles **2D → ISO → CPU saver**. CPU saver hides the toolpath and redraws once
  a second, leaving more time for motion. After an E-stop or limit the view switches to
  ISO by itself.
- **M6 / M0**: the machine holds with a message ("change to T3, then Start"). Do the
  change, **F1 Start** continues.
- **F2 Pause** = feed hold (F1 resumes). **F3 Reset** stops; the lines done so far are
  remembered.

### 3.6 Resume after a stop
Run → **Tab** → Program. Pick the line (**Y±** one line, **X±** ten), **Select**. The bar
shows what Start will restore (WCS, F, S, tool, coolant) and how it approaches: Z up, XY
across, then down. **F1 Start**.

## 4. When the machine stops on its own

### 4.1 Controlled stop (no lost steps)
*Settings › Homing & limits › Controlled stop on Reset, E-stop, limit* — **on by default**.
Reset, the E-stop and a limit switch all **brake along the path** at the axes' maximum
deceleration, so the motors don't lose steps and the position stays valid: no re-homing,
no re-zeroing. The machine runs on by the **braking distance**: about **1 mm** at full
speed (measured). **Mount limit switches with at least that much room before the hard
stop.** Switch it off and the stop is abrupt (≈ 0.3–0.5 mm), but a fast-moving motor may
then lose steps.

### 4.2 E-stop
The E-stop is for **unexpected or unwanted movement**. It stops motion, spindle and outputs.
**The drives stay on**, so the axes keep their position and holding torque. Release the
button and the machine is ready again. Jog, or resume from a line. **To remove power,
switch the machine off.**

### 4.3 Limit switch
The banner shows e.g. `X+ LIMIT`. Only that direction is blocked. **Mode** → Jog and:

- **back off** the switch (X− here): the limit clears by itself when the switch releases;
- or move **sideways** (Y) or **up/down** (Z): never blocked.

**Two or three switches at once** (e.g. driving into a corner): each one stops the move and
blocks its own direction; the banner lists them all (`X+ Y− LIMIT · jog X− Y+ to release`).
LIMIT only goes away when the **last** switch is released.

A program that was running has stopped with its progress remembered: resume as in 3.6.
Which input each axis's switch is on: *Settings › Homing & limits › X/Y/Z limit/home switch
input* (this machine: IN02 / IN03 / IN04; 0 = no switch on that axis).

### 4.3a Soft limits and bench limits
Two software boxes keep the machine inside its travel, without touching a switch:

| | In force | Set in *Settings › Homing & limits* |
|---|---|---|
| **Soft limits** | once the machine is **homed** (before that, positions mean nothing) | *X/Y/Z soft limit − / +*, switch: *Soft limits* |
| **Bench limits** | **always**, homed or not: a work box, e.g. the part of the table you use, or a safe box on the bench while testing | *X/Y/Z bench limit − / +*, switch: *Bench limits* |

When both are on, the tighter one counts. What they do:

- **A program that would leave the box is refused** at Start, with the axis, the side and
  how far: e.g. *Program goes past the X+ bench limit on G54: reaches 675.8 mm, limit
  587.8*. TryCut is checked at its clearance height. Simulate still runs (it moves nothing)
  and says the program is outside.
- **A jog towards the edge slows down and stops exactly on it.** A step jog past the edge is
  shortened to end on the edge; a held (continuous) jog brakes at the axis' acceleration as
  it gets close. Measured on the controller: from 16 mm/s to a standstill within the last
  ~0.7 mm, landing on the edge to the step. The next jog that way is refused (*X+ is at its
  bench limit*); the other way is free.
- Goto 0, Park and Tool change are refused if their target is outside the box.

### 4.4 DRIVES OFF
Only when someone switched the drives off (Jog F3, or over the network). The banner says
who. **F3 Reset** on any screen switches them on again.

## 5. Macros
**Page R** → Macros. **Y±** pick, **Select / Enter / F1** run. **Reset** stops any macro.

| Macro | Does |
|---|---|
| Safe Z | Z up to work Z0 + Safe Z (only ever up) |
| Go to work zero | Z up, then X Y to the active work zero |
| Park | Z up, X Y to the park position, Z to its height *(homed machine only)* |
| Tool change position | Same, to the tool-change position *(homed machine only)* |
| Spindle warm-up | Spindle at the warm-up speed for the warm-up time, then off |
| Zero all | X Y Z of the active work offset = the current position |

Positions, speed and time: *Settings › Macros* (machine coordinates).

## 6. Settings
**Page R** → Settings. Left: groups, right: values. Quick press / Modify / Enter starts
editing. On/off, lists and small numbers change in place with Y± (or hold + wheel); larger
numbers open the digit editor. **Enter accepts, Cancel discards.** Moving past the first
or last value goes back to the groups, so the pendant alone can do everything.

- **Grey values don't apply** right now, and the reason is shown (e.g. "API is off"). They
  can't be edited until the setting they depend on is changed.
- Values marked **idle only** (machine settings) can't change while anything moves.
- **F2** restores this machine's original values. **F4** exports the settings to USB.

Useful ones: *Display › Default toolpath view* (2D / ISO / CPU saver) ·
*Jog & touch-off › Safe Z* · *Homing & limits › Controlled stop* ·
*Network & API › Allow remote START* (on by default; switch off if you don't want a
network client to start programs) · *Pendant › Reverse wheel direction*.

## 7. The pendant (optional)

| Input | Does |
|---|---|
| Wheel, dead-man released | **Moves the focus**: list items, settings, digit cursor, view |
| Wheel, dead-man held, switch OFF | **Changes** the focused value; while running: overrides |
| Wheel, dead-man held, switch X/Y/Z/4 | **Jogs** that axis 0.001 / 0.01 / 0.1 mm per count (refused while running) |
| Quick press of the dead-man (switch OFF) | **Enter** on the focused item |
| Pendant E-stop | Same as the panel E-stop (§ 4.2) |

## 8. Network
The controller serves a browser panel and the API on port 8080
(`http://<controller-ip>:8080/`). Every panel action is also an API call; see
[api-reference.md](api-reference.md). Programs can be uploaded and started remotely. A remote
start always counts down on the panel first, and Reset cancels it.

## 9. Known limits (2026-10-21)
- **Not yet tested with drives or motors connected**, nor with real limit/home switches
  or a probe (probing is not configured). Limit handling was tested through the real
  input path with the inputs' level inverted, not with a switch.
- Loading a very large file (64 000 lines) takes ~4 s, and the panel freezes for that time.
- G84 tapping and tool length compensation (G43) are not supported.
- `G28` without axis words sends all axes to machine zero **together**, not Z first, as
  written in the program. Check CAM output before the first real run.
