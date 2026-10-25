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
API answer at `http://<that IP>:8080/`. This is the controller's own update route: the
vendor's manual (section 3.4.3) updates its software the same way.

**Back to the original software:** download the newest DDCS V4.1 software from Digital Dream
at [www.ddcnc.com › Downloads › Software & firmware](http://www.ddcnc.com/?m=home&c=Lists&a=index&tid=106&lang=en)
(newest at the time of writing: *DDCS V4.1 (2025-04-04) software*), unpack it, copy its
`ddcsv4` folder to an empty FAT32 stick, power off 10 s, insert, power on, wait ~30 s, remove
the stick. Details in [usb-install.md](usb-install.md#restoring-the-original-software).
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
  (READY, RUN, HOLD, TOOL CHANGE, E-STOP, LIMIT, DRIVES OFF, SIMULATE…).
- **Soft keys F1–F7** along the bottom: their labels change per screen. **F7 is always
  Page R**: it opens the **screen menu** (below). With **Shift** lit the labels turn amber
  and show the second functions.
- **Right strip**: what **Tab**, **Shift** and **Mode** do right now.
- **Toasts** (white box near the bottom): the result of what you just did, or why a key
  was refused. Nothing is refused silently.

## 2. Keys

| Key | Does |
|---|---|
| **Mode** | Run ↔ Jog. Jog is locked while a program runs or holds (not during a tool change). |
| **F7 Page R** | Opens the **screen menu**: the soft keys become the screens, press one to go there (see below). Page R again or Cancel closes it |
| **Shift + F7** | Back to the last screen |
| **Tab** | Next pane on the screen (e.g. Toolpath ↔ Program) |
| **Shift** | ALT layer for the next key (lit while active): touch-off, second functions (below) |
| **Y± / X±** | Move in lists and tables; jog in Jog |
| **Enter / Select / Modify** | Open / accept / edit the focused item |
| **Cancel** | Back / discard |
| **F1 Start** | Start or resume the program |
| **F2 Pause** | Feed hold on **every** screen while something moves |
| **F3 Reset** | Stop on **every** screen. Also switches the drives on after DRIVES OFF |

**The screen menu** (Page R, then):

| F1 | F2 | F3 | F4 | F5 | F6 | F7 |
|---|---|---|---|---|---|---|
| Run | Macros | Tools | Files | Work | Settings | close |

Jog isn't in the menu: **Mode** goes there directly. While something moves, F2 and F3 stay
**Pause** and **Reset**, menu or not. The menu closes after any key, or by itself after 8 s.

**Second functions (Shift, then the key)**:

| Screen | F1 | F2 | F3 | F4 | F5 | F6 |
|---|---|---|---|---|---|---|
| Run | Start from the line picked in the listing | **WCS**: next work offset (G54 → G55 …) | Rewind to line 1 (asks first) | View: fit / reset | **Goto line** (below) | Coolant on/off |
| Jog | Home | **Safe Z** (Z up only) | Drives | Step size back | WCS back | Coolant on/off |
| Work | | | | **Job: none** (clears every JOB mark) | | |

A running program still pauses with F2 and resets with F3, Shift or not.

## 3. Daily procedure

### 3.1 Power up
1. Switch on. If the E-stop is pressed, release it. The banner tells you what's needed.
2. **Mode** → Jog. **F1 Home** if you want soft limits (not required). Reset stops homing.
3. *Optional:* **Keep the homed state after a restart** (*Settings › Homing & limits*, off by
   default). The position is saved 2 s after every stop. After a restart or power cut,
   Sea Otter is still homed if the machine was at rest when the power went. It is not homed
   if the power went while it was moving. For a quick reboot when you don't need the last
   hundredth: the drives were reset too (a small *clunk* is normal), so expect up to
   1/100–1/50 turn, **0.05–0.1 mm on a 5 mm screw**. Home again for precision work.

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
4. Check with **Goto 0** (Jog F2): Z up to safe height, then XY to 0. A blue banner shows it
   while it moves. **F2 Pause** holds it and **F1 Start** continues it, from any screen.
   **F3 Reset** ends it. **Shift + F2** on Jog is *Safe Z*: Z up only.
5. Several parts: **Jog F5 WCS** picks G55, G56…; on **Work** mark them **F4 In job**.
   Start then cuts **per tool**: everything the first tool does, on every JOB part in turn,
   then **one** tool change, then the second tool on every part, and so on. So 4 parts
   with 3 tools means 3 tool changes, not 12. Between parts Z goes up to the safe height.
   **Shift + F4** on Work clears the job. *Settings › Tool change › Several JOB parts: cut*
   switches to **one part at a time**: each part is finished with all its tools before the next
   one starts (a tool change per tool per part, but every part is done early). The colouring
   in the view and the resume part (Modify) follow the chosen order.

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
- **Run time** (top right of the toolpath): *estimate* while idle (the whole program, every
  JOB part), *remaining* while it runs, holds or waits at a tool change. It follows the feed
  override at once (rapids follow it only below 100 %). It is worked out the way the motion
  planner moves: acceleration, corners, the controller's speed limit, dwells and the spindle
  spin-up wait, and from where the machine stands now. It is exact: the controller replays the
  program through its own motion control in the background. On the controller a 4-minute
  engraving was estimated at 261 s and ran 260.9 s. Right after loading, a first figure from a
  simpler model is shown (a little long); for very long programs the exact one follows within
  minutes. *…* means it is still being worked out. The operator's tool changes are not in it;
  *+ N tool changes* is shown below it instead.
- **F4 View** cycles **2D → ISO → G-code view**. The G-code view draws no wireframe at all:
  the program listing fills the whole right side (about 18 lines), and the screen redraws
  less often, leaving more time for motion. It's the best choice for heavy programs. After an
  E-stop or limit the view switches to ISO by itself.
- **Too heavy?** If drawing the wireframe takes time from motion while something moves,
  Sea Otter first redraws it less often (an amber note in the view says so), then switches to
  the G-code view by itself, with an amber line that stays until you press F4.
- **The preview** always fits and centres the whole picture: the program, the work zero's
  axes, and small axes labelled G55, G56… at the zero of every other JOB part. The thin box
  around the toolpath is the program's size (there is none without a program). The grid lies
  on the **machine's Z floor** (the Z soft limit −) in the ISO view. Its coarse lines are 1 cm
  when the picture is under 10 cm, 5 cm up to 50 cm and 10 cm up to 1 m. Its fine lines, barely
  lighter than the background, are a tenth or a fifth of that. Both steps are shown bottom right.
- **No program loaded**: the listing says so. Enter or Select picks nothing.
- **M0 / M1**: the machine holds with a message; **F1 Start** continues.
- **M6** (tool change): see § 3.7.
- **F2 Pause** = feed hold (F1 resumes). **F3 Reset** stops; the lines done so far are
  remembered.

### 3.6 Resume after a stop
**Start continues where the program stopped.** After Reset, an E-stop or a limit, the
stopped line becomes the resume point. Start rebuilds the state there (WCS, F, S, tool,
coolant) and approaches it: Z up, XY across, then down.

- **Back to the beginning** takes **two** Resets. The first asks *"Go back to the
  BEGINNING (line 1)?"* in an amber banner. A second Reset within 5 s confirms; any other key keeps
  the line.
- **Another line**: Run → **Tab** → Program, pick it (**Y±** one line, **X±** ten), **Select**.
  Or **Shift + F5 Goto line**: type the line number in the digit editor. Beside it, a list
  shows good places to continue from. These are the program's **sections** (comment lines that start a
  block, like CAM operation names). Without sections, it lists the **retracts to its usual
  safe height** (the highest Z it goes up to most often), each with its line number. Enter goes
  there, Cancel doesn't.
- **What you skip shows in orange.** While no program runs, the lines before the picked one
  (the one you scroll to in the Program pane, or the resume point) are dark orange, in the
  listing and in the toolpath. A ringed orange dot marks where the picked line's move ends,
  so you see where Start will go. Once the program runs, the skipped part stays a dimmer
  brown, and what is cut turns teal as before.
- **Several JOB parts**: every part gets its own copy of the toolpath. In the Program pane,
  **Modify** picks the part to resume on (the footer says *Resume @ line N on G55*). The
  colouring follows the job order per tool. Up to the picked line's tool section, every part
  counts as done. Parts before the picked one have that whole tool section done. The picked part
  is done up to the line, and later parts only up to the section's start. Drawing N parts
  costs N times the drawing time on a redraw (the G-code view avoids it).
- **After a restart or a power cut** the program comes back loaded, with the resume point
  near where it was. While a program runs, Sea Otter saves its line once a minute, so the
  resume point can be up to a minute behind. Check the line before you press Start. It never starts
  by itself.

### 3.7 Tool change (M6)
*Settings › Tool change › On M6* chooses what an `M6 T…` in the program does:

**Pause: change by hand** (default):
1. The spindle stops, Z goes up to the **safe height** (or, with *Change the tool at ›
   Tool-change position*, to that position in machine coordinates; needs a homed machine).
2. The state becomes **TOOL CHANGE** and the amber banner says which tool to put in.
3. **Jogging is allowed**: **Mode** → Jog, jog, change the tool, and touch off Z with the new
   tool (**Shift + Z−**) if its length differs. In a multi-part job, a Z touch-off on one part
   is applied to the other parts too: it's the same tool.
4. **F1 Start** shows **"Back to the program?"**: a straight line to the position the
   program stopped at (at least the safe height, then straight down if lower), the speed,
   and the tool. **F1 Start or Enter** goes, **Cancel** stays. Jogging after the question
   cancels it: Start asks again with the new route.
5. The return speed is *Settings › Tool change › Return at* (% of rapid, default 50 %). The
   spindle and coolant come back on as the program had them before the return move starts.

**F3 Reset** during a tool change stops the program. Its progress is remembered, as with any stop.
If the tool asked for is already in, nothing happens. Simulate never stops for a tool.

**Run a tool-change macro**: Sea Otter runs a G-code file instead of stopping, then goes
back to the program (Z up first, then across, then down). It looks in the macro folders
(`/local/macros` first, then `/ddcsv4`) for:
- `toolchange-T3.nc`: **one file per tool** (copy one and adjust it per tool), or else
- `toolchange.nc`: **one file for all tools**, using `{tool}` for the tool number.

Besides the macro placeholders (§ 5), a tool-change macro can use `{tool}` (the new tool),
`{tool.prev}` (the one in now), `{tool.dia}` and `{tool.len}` (from the Tools table). Example:
```
(name: Tool change)
(info: to the rack, slot = tool number)
(needs: homed)
G90
G53 G0 Z{macro.tc_z}
G53 G0 X{macro.tc_x} Y{macro.tc_y}
M0 (put in T{tool}, then Start)
```
Without either file, it falls back to changing by hand.

**Ignore M6**: the tool number is noted and the program runs on.

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
| **Soft limits** | once the machine is **homed** (before that, positions mean nothing). Default: the machine's **400 × 200 × 100 mm** (X 0…400, Y 0…200, Z −100…0 below the Z switch) | *X/Y/Z soft limit − / +*, switch: *Soft limits* |
| **Bench limits** | **always**, homed or not: a work box, e.g. the part of the table you use, or a safe box on the bench while testing | *X/Y/Z bench limit − / +*, switch: *Bench limits* |

When both are on, the tighter one counts. What they do:

- **A program that would leave the box is refused** at Start — rapid moves count, not only
  cutting moves (a `G28` to machine zero can therefore be refused) — with the axis, the side and
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
Macros are **small G-code files** — nothing is built in. The Macros page lists every macro
file it finds, and you can add, change or remove them yourself.

**Running one:** **Page R** → Macros, **Y±** pick, **Select / Enter / F1**. The macro is
*armed*: Sea Otter switches to the **Run page**, the program pane shows the macro's lines and
a teal banner says *MACRO · Park armed*. Then it behaves exactly like a program:

| Key | Does |
|---|---|
| **F1 Start** | runs the macro (its values are filled in at this moment, see below) |
| **F2 Pause** | holds it; **F1 Start** continues |
| **F3 Reset** | stops it halfway (or cancels it before Start) |

When the macro ends — finished, Reset, E-stop or a limit — **your loaded program is back**,
with its resume line and its Run / TryCut / Simulate setting as they were. A macro that
would leave the soft or bench limits is refused at Start, like a program.

**Where macros come from:**

| Folder | What |
|---|---|
| `macro-*.nc` in the app folder (`/ddcsv4`) | the package's set; installed and updated with the USB package. The package brings: Safe Z, Go to work zero, Park, Tool change position, Spindle warm-up, Zero all |
| **`macros`** folder on the **CNCDISK** network share (`/local/macros`) | **your own**: copy `.nc` files there from a PC, no USB stick or restart needed (F4 Rescan on the Macros page) |

**Writing one:** plain G-code, with optional header comments on the first lines, and
`{placeholders}` that are filled in when you press Start:

```
(name: Park)
(info: Z up, X Y to the park position, Z to its height)
(needs: homed)
G90
G53 G0 Z{max:machine.z,macro.park_z}
G53 G0 X{macro.park_x} Y{macro.park_y}
G53 G0 Z{macro.park_z}
```

| | |
|---|---|
| `(name: …)` | the name on the Macros page (default: the file name) |
| `(info: …)` | one line under the name |
| `(needs: homed)` | refused until the machine is homed (for machine coordinates) |
| `{jog.safe_z}`, `{macro.park_x}`, … | the value of any setting (the keys in [api-reference.md](api-reference.md#settings-keys)) |
| `{machine.x}` `{work.z}` … | the current position, machine or work coordinates (x y z a) |
| `{max:A,B}` `{min:A,B}` | the larger / smaller of two numbers or names, e.g. `{max:work.z,jog.safe_z}` = "Z up to the safe height, never down" |

*Settings › Macros* holds the values the package's macros use (park and tool-change
positions, warm-up speed and time); your own macros can use them too.

## 6. Settings
**Page R** → Settings. Left: groups, right: values. Quick press / Modify / Enter starts
editing. On/off, lists and small numbers change in place with Y± (or hold + wheel); larger
numbers open the digit editor. **Enter accepts, Cancel discards.** For a value that may be
negative, **Modify** flips the sign (or X− past the leading zero onto the ± cell, then Y±).
While the editor is open, the keys that do nothing are greyed out. F2/F3 still pause/reset
anything that moves (the edit is dropped). One step past the last
value (or before the first) shows an orange **← back to the groups** line, and the group on
the left gets an orange arrow: Enter or a quick press there goes back to the groups, one step
the other way returns to the list. Scrolling to the end never leaves the group by itself, and
the pendant alone can do everything.

- **Grey values don't apply** right now, and the reason is shown (e.g. "API is off"). They
  can't be edited until the setting they depend on is changed.
- Values marked **idle only** (machine settings) can't change while anything moves.
- **F5 Hint** shows a few sentences on what the focused setting does, in a box on the
  right (it moves out of the way of the focused row). F5 again hides it.
- **F2** restores this machine's original values. **F4** exports the settings to USB.

Useful ones: *Display › Default toolpath view* (2D / ISO / G-code view) ·
*Jog & touch-off › Safe Z* · *Homing & limits › Controlled stop* ·
*API* (below) · *Pendant › Reverse wheel direction* ·
*Tool change* (§ 3.7: by hand / macro / ignore, where, return speed).

**API** (*Settings › API*): the first line, **ENABLE the REST API endpoint**, switches the
whole network API on or off. Under it, each kind of action can be allowed or not. Reading,
programs, offsets, tools, settings, overrides and panel messages are on. **Starting programs,
jogging, moves (home, Goto 0, Safe Z), spindle, coolant, drives on and panel keys are off**
until you switch them on here. These switches can only be changed on the panel. Details:
[api-reference.md](api-reference.md#permissions).

**I/O overview** (Settings, **F6**): every input IN01–IN18 with its level, what is wired to it
and whether that function is active (amber). Also shown: the outputs, the spindle voltage, the pendant
(E-stop, dead-man, axis switch, multiplier, wheel counts, raw register), the panel keys held
right now, the LEDs and the FPGA position counters. **F6** or **Cancel** goes back to Settings.

## 7. The pendant (optional)

| Input | Does |
|---|---|
| Wheel, dead-man released | **Moves the focus**: list items, settings, digit cursor, view |
| Wheel, dead-man held, switch OFF | **Changes** the focused value; while running: overrides |
| Wheel, dead-man held, switch X/Y/Z/4 | **Jogs** that axis 0.001 / 0.01 / 0.1 mm per count (refused while running) |
| Quick press of the dead-man (switch OFF) | **Enter** on the focused item |
| **Jog screen**: dead-man held, switch X/Y/Z/4 | That axis's row lights up in a soft amber (on Jog and Run) and the jog pane is focused: only then does the wheel jog |
| **Jog screen**: dead-man held, switch OFF | The **Jog setup** pane is focused: the wheel changes the setup (step, WCS, …), never jogs |
| Pendant E-stop | Same as the panel E-stop (§ 4.2) |

The wheel never acts as a jog key. Before this fix, turning it on the Jog screen with the
switch on OFF and the step on *continuous* started a Y jog that kept going. Also: any key
press ends a continuous jog, and touch-off waits until the axis stands still.
**While an edit box is open** (the digit editor, Goto line, a setting being changed, the
tool-change question), the axis switch is ignored. The wheel then works as with the switch
on OFF: it edits, and nothing moves.

**Pendant dead, E-stop "pressed"?** If every pendant line reads high (I/O screen: raw
`0x3FF`), the pendant is unplugged, its cable is loose or it has no power. The E-stop line then
reads "pressed" too, and the banner says so. Nothing in the software can bring the buttons
back, because they all read "not pressed". The motion controller itself also refuses to step while
that line reads "pressed" (measured 2026-10-24), whatever *Pendant › Pendant E-stop enabled* says.
Check the plug and the cable.

## 8. Network
The controller serves a browser panel and the API on port 8080
(`http://<controller-ip>:8080/`). Every panel action is also an API call; see
[api-reference.md](api-reference.md). What the network may do is set in *Settings › API*
(§ 6). A remote start, once allowed, counts down on the panel first, and Reset cancels it.

## 9. Known limits (2026-10-21)
- **Not yet tested with drives or motors connected**, nor with real limit/home switches
  or a probe (probing is not configured). Limit handling was tested through the real
  input path with the inputs' level inverted, not with a switch.
- Loading a very large file (64 000 lines) takes ~4 s, and the panel freezes for that time.
- G84 tapping and tool length compensation (G43) are not supported.
- `G28` without axis words sends all axes to machine zero **together**, not Z first, as
  written in the program. Check CAM output before the first real run.
