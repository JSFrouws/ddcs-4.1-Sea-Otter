# Operator procedures, key model and the machine state machine

_2026-10-18. Second design pass after reviewing the first mockup against the real
panel photo. **Supersedes the navigation model in [operator-ux.md](operator-ux.md)**
(11 screens, Shift+F-key page jumps). Mockup: [../mockup/ui-proposal.html](../mockup/ui-proposal.html)
(run `mockup/start-ui.bat`)._

## What "Page R" is

The vendor's own message catalogue settles it
(`raw/sysdisk-2026-10-15/msg-eng` #207-227). The vendor app has three soft-key sets
for F1-F7, and **F7 is always the "next page" key**:

| Page | F1 | F2 | F3 | F4 | F5 | F6 | F7 |
|---|---|---|---|---|---|---|---|
| Main | Start | Pause | Reset | View | MpgGuide | Spindle | **File...** |
| File | Emulation | Copy | Edit | New | Delete | Rename | **Param...** |
| Param | ParamType | Search | Visible | Export | Import | Logout | **Main...** |

So **Page R = page rotate** (Main → File → Param → Main), and the vendor already
treats F1-F6 as per-page soft keys whatever their silkscreen says (F5 is labelled
`TryCut` but shows `MpgGuide`). The new UI keeps both ideas.

## Design review (what changed and why)

The first mockup had 11 screens, Shift+F-key jumps, a "Tab = focus next pane" that
was never specified, and Cancel doubling as "back to Run". Too many concepts for a
panel with no touch. This pass keeps **one meaning per key class**:

| Key | One job |
|---|---|
| **Mode** | Toggle **Run (AUTO) ↔ Jog (MANUAL)** from anywhere. Refused while a program runs/holds or a move is in progress. |
| **Page R** (F7) | Rotate the setup pages **Run → Files → Work → Tools → Settings → Run**. Shift+Page R goes back. |
| **Tab** | Move focus to the **next pane on this screen**. The focused pane has a cyan outline and **owns the arrow keys**. Shift+Tab goes back. |
| **F1-F6** | Soft keys, labelled per screen on the LCD above each key. **While anything moves, F2 = Pause and F3 = Reset on every screen** - their printed meaning. |
| **Arrows** (X−/X+/Y+/Y−, Cancel=Z+, Enter=Z−, Select=A+, Modify=A−) | Jog on the Jog screen; navigate/edit in the focused pane elsewhere. |
| **Shift** | One-shot modifier (Shift LED). Means "the printed second word": GOTO 0 / ZERO / HOME / PROBE on Run, **touch-off** on Jog, "reverse" for Tab/Page R. |

Six screens instead of eleven:

| Screen | Panes (Tab) | F1-F6 when idle |
|---|---|---|
| **Run** (AUTO) | Toolpath view · Program listing | Start · Pause · Reset · View 2D/ISO · TryCut arming · Spindle |
| **Jog** (MANUAL, via Mode) | Jog keys · Jog setup (edge Ø, step, WCS, feed) | Home all · Goto 0 · Drives on/off · Step · WCS · Spindle |
| **Files** | File list · Preview (rotatable) | Load · Load+simulate · Source |
| **Work zero & probe** | Offsets table G54-G59 · Probe routines | Activate · From pos · Edit · In job · Clear · Probe |
| **Tools** | Tool table | Load tool · Edit · Copy Ø to edge-Ø |
| **Settings** | Categories · Values (I/O & diagnostics is a category) | Save · Revert · Edit · Export |

Home & Drives moved into Jog's soft keys; I/O moved into Settings; Macros were
dropped from the panel (the API and the soft keys cover them).

**Tab / Shift / Mode labels on the LCD.** These three keys sit directly right of the
LCD (photo: Tab at 27 %, Shift at 51 %, Mode at 75 % of its height), so the LCD's
right edge carries a 112 px strip with one label pointing at each key, exactly like
the bottom soft-key bar: Tab shows the pane it goes to next (`→ Program`, `pane 1/2`),
Shift shows what it does here (`touch-off · edge Ø12.00`, amber while latched),
Mode shows `→ JOG` / `→ RUN` or `JOG locked`.

## Machine state machine (no shortcuts)

All inputs - panel keys, pendant wheel, API/agent - are turned into **commands**
and handed to a single dispatcher `act(cmd, arg, src)`. It checks a state table,
then runs the command's effect and the transition. **Nothing else may change the
machine state or command motion**, so no screen, key combination or API call can
skip a guard. (Mockup: [../mockup/ui/ui-machine.js](../mockup/ui/ui-machine.js),
effects in `ui-effects.js`; the real app keeps this exact shape, in C.)

```
            estopIn (from any state)          estopOut (drives stayed on)
   ┌──────────────────────────────────────────► ESTOP ──────────► IDLE
   │
   │   drivesOff
 IDLE ◄──────────────── drivesOn ─────────────── OFF
  │ ▲ ▲ ▲
  │ │ │ └─ done/stop/pause ── HOMING ◄── home
  │ │ └─── done/stop/pause ── MOVE   ◄── gotoZero / macro
  │ └───── done/stop ──────── RUN ◄──── start ──┐
  │                           │ pause    ▲ start │
  │                           ▼          │       │
  │                          HOLD ───────┘       │
  │   limitHit (IDLE/RUN/MOVE)                   │
  └──────────────────────────► LIMIT ─ limitClear (switch released) ─► IDLE
```

| State | Accepted commands (everything else is refused with a reason) |
|---|---|
| ESTOP | release E-stop (→ IDLE: the E-stop never switches the drives off); edit tables/settings |
| OFF (drives off) | drives on, load file, edit tables/settings |
| IDLE | jog, touch-off, goto 0, macro, home, probe, start, spindle, drives off, load file, edit, overrides |
| MOVE / HOMING / PROBING | pause (= stop), stop |
| RUN | pause, stop, overrides (feed - rapid follows it - and spindle) |
| HOLD | start (resume), stop, overrides |
| LIMIT | jog **except the tripped direction**, stop, drives off, edit tables/settings, overrides |

Rules this gives, without any per-screen special cases:

- **While a program runs or holds, every directional control is refused**: arrow
  jogs, touch-off, goto 0, home, probe, the pendant's X/Y/Z/4 wheel, and the same
  calls over the API. Mode won't even open Jog. Only Pause, Reset and the
  overrides (pendant OFF + hold, API) work. If a program is started while Jog is on
  screen (e.g. by an agent), the UI switches to Run.
- HOLD is deliberately strict: to touch anything, Reset. The work-done log means
  "Reset, fix, resume from line" is two key presses, so nothing is lost.
- A hard limit blocks only that axis direction; jogging off the switch clears it
  automatically ([operator-ux.md](operator-ux.md) § Limit policy).
- Machine settings (jog feed, safe Z…) are marked "idle only" and need
  `editSetting`; display settings (decimals, view) can change any time.
- Remote START needs `Settings › Network & API › Allow remote START` (on by
  default, switch it off there), then counts down on the panel (default 5 s, Reset cancels).

## Numbers: the digit editor

Every number is edited one digit at a time, like the vendor software. An amber
**underline** marks the digit under the cursor.

- **X− / X+** move the cursor; moving left of the highest digit adds a digit (up to 5).
- **Y+ / Y−** change that digit by one, **wrapping 0↔9 without carrying**:
  `0.0`, two left, Y+ → `100.0`; Y− instead → `900.0`.
- Signed values have a `+/−` cell left of the digits (Y± flips it).
- **Enter** accepts, **Cancel** discards, **Shift+ZERO** clears to 0.
- With the pendant: **wheel** moves the cursor, **hold + wheel** changes the digit,
  a **quick press** of hold accepts (see § Pendant below).

## Pendant: optional, but it can drive the whole UI

The pendant is optional (Settings › Pendant › *MPG pendant fitted*). Implemented in the device app: [../app/](../app/README.md); every action
also has a panel key. When fitted, it follows one rule everywhere:

| Input | Does |
|---|---|
| **Wheel, hold released** | **Move** - never changes anything: digit cursor in the editor, focus in lists/tables/settings/program, turn the toolpath view |
| **Wheel, hold pressed, switch OFF** | **Change** - the digit under the cursor; the focused setting/field value (enums, step, WCS); tilt the view; scrub the program (X1 line, X10 ten lines, X100 section); fast scroll elsewhere. **While running: feed % / spindle % in 2 % steps / feed % in steps of 10** (X1 / X10 / X100), up to 300 %; rapid follows the feed % down and is capped at 100 % |
| **Wheel, hold pressed, switch X/Y/Z/4** | **Jog** that axis 0.001 / 0.010 / 0.100 mm per count - through the state machine, so refused while running |
| **Quick press of hold** (< 0.4 s, no wheel counts, switch OFF) | **Enter** on the focused item: open the editor / toggle / accept the edit / load the file / set the resume line |

So editing a number from the pendant is: turn to the field, quick press, turn to the
digit, hold + turn to change it, quick press to accept. The quick-press time is a
setting (default 400 ms).

This also matches the hardware: the axis and multiplier switches are powered through
the dead-man ([mpg-pendant-io.md](mpg-pendant-io.md)), so with hold released the app
can't read them at all - the wheel count alone can only safely mean "move the
cursor". A press is detected as "some multiplier bit appears in `0x3D`", a release
as "all switch bits gone". The quick press is only recognised with the switch on
OFF (on X/Y/Z/4 a press is just the start of a jog). The UI only ever says OFF: positions 5 and 6 have no wires and read exactly like OFF, so the code never distinguishes them.

**Decimals** (Settings › Display): *Position decimals* `0.0 / 0.00 / 0.000` (X Y Z
A everywhere: DRO, offsets, touch-off values; default 0.000) and *Tool size
decimals* `0.0 / 0.00 / 0.000` (Ø and length in the tool table and edge Ø; default
0.00). The editor uses the same number of decimals as the display.

## Procedures

Key names as printed on the panel. "⇧X" = Shift, then X.

### 1. Power up and home
1. Release the E-stop if needed. The banner says what's next.
2. **Mode** → Jog. If the banner says DRIVES OFF: **F3 Drives**.
3. **F1 Home** (order Z, X, Y, A from the saved settings). Reset stops it.

Not homed is allowed (no forced homing) - soft limits just stay off and the top bar
shows `Homed —`. From Run: **⇧ENTER (HOME)** does the same.

### 2. Select a program
1. **Page R** → Files. **X−/X+** switch Local / USB / Network share; **Y±** pick.
2. **Tab** to the preview to turn it around (X± turn, Y± tilt, Z± zoom) if you want.
3. **F1 Load** (or Select) → back on Run with the file loaded. **F2** loads and arms
   Simulate in one go.

### 3. Zero a workpiece (edge finder or the cutter itself)
1. **Mode** → Jog. **Tab** → Jog setup; **Y±** to *Edge / tool Ø*, **Enter**, dial
   it in with the digit editor (e.g. `12.00`), **Enter**. Or Tools › **F3** copies
   the focused tool's Ø. **F5** picks which WCS you are setting (G54…).
2. **Tab** back to Jog keys. Jog X+ until the tool touches the part's left edge.
3. **⇧X+** → X becomes **−6.000** (tool centre is Ø/2 left of the edge). While
   Shift is held, the DRO shows the value each touch-off would set.
4. Shift stays lit: pressing **X+ again sets X = 0** instead (e.g. you're at the
   centre already, or you want the tool-centre convention).
5. Same for Y (**⇧Y+** for the front edge → −6.000). For Z touch the top and **⇧Z−
   (ENTER)** → Z = 0. With no Ø set, every touch-off sets 0.
6. **⇧GOTO 0** (from Run) or **F2 Goto 0** (Jog) to check: Z to safe height, then XY to 0.

### 4. Several workpieces
Give each part its own WCS: in Jog **F5** → G55, touch off as above; repeat. On
**Work** mark the fixtures **F4 In job**. The top bar shows `job 2 parts`; Start
runs the program on each JOB fixture in turn (safe Z between parts). The offsets
table on Work is also where any value can be typed in (Enter → digit editor) or
taken from the current position (**F2 From pos**).

### 5. Verify, run, pause
1. Run screen: **F5 TryCut** cycles the arming *Run → TryCut (spindle off, Z fixed at
   the clearance level) → Simulate (no motion, DRO orange)*. The ARMED tag on the toolpath shows it.
2. **F1 Start**. During the run: pendant OFF + hold, X1 = feed %, X10 =
   spindle % (2 % per detent, up to 300 %), X100 = feed % in steps of 10. **Rapid follows feed**: lowering the feed % lowers
   the rapid % with it, raising it stops at rapid 100 % (never 101 %). **F4** cycles 2D / ISO wireframe / CPU saver (no preview, 1 redraw/s while moving); Tab → Toolpath
   and the arrows turn/zoom it (view only - no motion).
3. **F2 Pause** = feed hold, **F1** resumes. **F3 Reset** stops; the work done is
   remembered.

### 6. Resume after a stop, a broken tool or a limit
1. Run → **Tab** → Program. **Y±** one line, **X±** ten lines, or the pendant
   OFF + hold X1/X10/X100 scrubs lines / tens / sections. Done lines are dimmed.
2. **Select A** on the line to restart from. The bar shows what Start will restore
   (WCS, F, S, tool, coolant) and the approach (safe Z, then XY, then down).
3. **F1 Start**. (Strategy and logs: [workflow-and-resume.md](workflow-and-resume.md).)

### 7. Limit switch hit, E-stop, Reset: the controlled stop
With *Settings › Homing & limits › Controlled stop* on (default), all three brake
along the path at the axes' max deceleration: **no lost steps, no re-homing**. The
machine runs on past the switch by the braking distance v²/2a (≈ 0.7 mm at
1200 mm/min and 300 mm/s²) - mount switches with that much room before the hard
stop. With it off the stop is abrupt and a fast axis can lose steps.

Banner `X+ LIMIT`; only X+ is blocked. Mode → Jog and get off it any way that
makes sense: **X− back** off the switch (the limit clears itself), **sideways**
(Y), **up/down** (Z) - those were never blocked. The program was stopped and its
work done remembered: resume as in 6. If the view was on *CPU saver* it has
switched to ISO so you can see where the tool is.

**E-stop**: motion, spindle and outputs stop; the drives stay on (it is for
unwanted motion - to remove power, switch the machine off). Release it and the
machine is IDLE again; jog or resume as above.

### 7a. Macros
**Page R** → Macros. **Y±** pick, **Select / Enter / F1** run: Safe Z, Go to work
zero, Park, Tool change position, Spindle warm-up, Zero all. Park and tool-change
positions are set in *Settings › Macros* (machine coordinates, homed machine).
Reset stops any macro.

### 8. An AI agent prepares and starts a job
An agent (or any script) talks to the API ([ethernet-api.md](ethernet-api.md)):

1. It uploads and loads the program (`POST /program/load`) and posts an
   instruction list (`POST /panel/instructions`). The panel shows it as a card on
   Run, and **ticks items off by watching the machine state**: homed, file loaded,
   stock clamped (operator presses Select A), X/Y/Z touched off.
2. The operator does the physical steps with the procedures above. The agent can
   read everything (`GET /status`, WebSocket) and can jog, home or probe like the
   panel, through the same state machine.
3. **Start**: `POST /run/start` is accepted as **202** (remote START is on by default),
   the panel shows `REMOTE START in 5 s` (configurable) and **Reset cancels**. An owner who
   distrusts remote agents switches *Settings › Network & API › Allow remote START* off;
   then the request gets **403 remote start disabled** and the card tells the operator to
   press **Start**. Either way the state machine still requires IDLE (or HOLD for
   a resume) and a loaded program.

## Rendering on the unit (and the "is the vendor app sluggish?" question)

Assume low performance and design for it. The i.MX28 is an ARM926EJ-S at ~454 MHz
with **no GPU and no floating-point unit** (soft-float), and a 1024×600×16 bpp
frame is 1.2 MB. That's plenty for flat 2D widgets, text and line-drawn toolpaths
**if only changed regions are redrawn**; it is not enough for anything filled,
blended or 3D-shaded.

Recommendation, lightest first:
1. **C, straight to `/dev/fb0`**, with a tiny widget layer (rectangles, bitmap
   fonts pre-rendered in RGB565, Bresenham lines) and dirty-rectangle updates. Least
   code between the key press and the pixels; same process as the motion backend.
2. **LVGL on the fbdev driver** if a ready widget set is preferred: MIT-licensed,
   designed for 100 MHz microcontrollers, supports RGB565 natively. A little more
   memory, much less custom code.

No Python/JavaScript/Qt on the unit: no FPU, and Qt Widgets on Linux 2.6.35 is a
large porting job for little gain. (The browser mockup is only for agreeing on the
UI.) Coordinates and the G-code interpreter should use fixed point (e.g. integer
nanometres or steps) to avoid soft-float cost in the inner loop.

Is the vendor app slow because of the hardware? Partly unknown - it hasn't been
measured. The hardware can redraw a DRO many times per second; typical causes of
a sluggish feel on this class of device are full-screen repaints per value change
and polling loops with sleeps. Measurable over SSH with `top` / `/proc/<pid>/stat`
while it runs (a follow-up, not done yet).

**3D**: no stock-removal simulation (too expensive). The toolpath view offers a 2D
plan and an **isometric wireframe** of the toolpath and the stock box, turned with
X±, tilted with Y±, zoomed with Z± (Cancel/Enter) when the view pane has focus,
redrawn only on a key press or when the run advances.

Turn and tilt are **clamped to the three principal views and never go past them**:
turn runs from the **X-Z** side view (Y vanishes) to the **Y-Z** side view (X
vanishes), tilt from side-on up to the **X-Y** top view (Z vanishes). The view can't
end up upside down or looking from behind, so the operator can't get lost in it;
the label under the view says when a limit view is reached.
