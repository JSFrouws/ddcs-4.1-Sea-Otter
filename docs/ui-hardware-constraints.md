# UI hardware constraints: screen, graphics budget, and input model

_2026-10-16. Supersedes the touchscreen/large-canvas assumptions in the first
draft of [ui-requirements.md](ui-requirements.md) (REQ-L1). Grounded in the
live-hardware findings: [ui-controls-map.md](ui-controls-map.md) (buttons/LEDs),
[mpg-investigation.md](mpg-investigation.md) + [connector-pinouts.md](connector-pinouts.md)
(pendant), [app-analysis.md](app-analysis.md) §4 (emWin), and the panel photos._

## Screen

- **Confirmed over SSH, 2026-10-18** (`cat /sys/class/graphics/fb0/virtual_size`,
  `/bits_per_pixel`, `fbset -s`, with the real app running — reading `/sys` doesn't
  need exclusive access the way the ioctl/FPGA probes did):
  ```
  $ ssh -i mockup/dropbear/ddcsv4_id_ed25519 root@192.168.1.200 fbset -s
  mode "1024x600-19"
      geometry 1024 600 1024 600 16
      rgba 5/11,6/5,5/0,0/0
  ```
  **1024×600 pixels, 16 bits per pixel (RGB565, 65 536 colors), driver `mxsfb`**
  (the i.MX28's own LCDIF controller — `stride` confirmed as `2048` bytes =
  1024px × 2 bytes, so there's no row padding to account for). Ratio is
  **1.707** (not 16:9's 1.778, and not 5:3's 1.667) — this is the common
  "WSVGA" panel size used on 7–8" industrial displays, a third option neither
  of the two guessed earlier in this doc was right about. The earlier estimate
  from a photo crop (≈16:9) is **superseded** by this direct read; corrected
  everywhere the ratio/pixel count is used below and in the mockup.
- **16bpp is a real constraint, not just a resolution number**: only 65 536
  colors are available (RGB565 — 5 bits red, 6 green, 5 blue). Flat fills and
  text are unaffected, but **smooth gradients can visibly band** at this depth,
  more so on blue/red channels (5-bit) than green (6-bit). Keep gradients
  subtle and short (a panel's background wash, not a long sweep), and don't
  rely on them for anything that needs to read precisely (e.g. a progress
  bar's fill should be a flat color or a very short-range gradient, not a
  wide hue sweep that would band).
- Design target used for the mockup: **1024×600**, letters slightly smaller
  than the vendor's — the user confirmed smaller text is acceptable for
  readability.
- It is a **plain LCD, driven as a Linux framebuffer** (`/dev/fb0`). There is an
  `/dev/input/event0` named "MXS touchscreen" on the board, but the panel is a
  **physical membrane keypad, not a touch panel** (user-confirmed; the event
  node is vestigial/unpopulated). **The UI must be fully operable from the
  physical keys and the pendant — no tap targets, no hover, no drag.**

## Graphics budget (what's feasible, what's asking too much)

The SoC is a **Freescale i.MX28 (ARM926EJ-S, ARMv5, ~454 MHz), no GPU**, Linux
2.6.35, software 2D rendering to the framebuffer. The vendor app uses **SEGGER
emWin 5.12** (confirmed: the `.rc` files are emWin `GUI_Builder` resources,
[app-analysis.md](app-analysis.md) §4) — i.e. flat 2D widgets, blitting, and
line drawing, all CPU-side.

**Feasible (do it):**
- Flat panels, text, big DRO digits, menus, soft-key bars, progress bars,
  status dots/LED mirrors — trivial, this is what emWin already does all day.
- **2D toolpath** (XY plan, plus an optional XZ/YZ side strip): polylines on the
  framebuffer. The vendor already backplots, so this is proven on this exact
  silicon. Pan and a few discrete zoom levels are fine.
- Redrawing a live position marker at a few Hz over a static backplot: cheap,
  if the backplot itself is drawn once and only the marker layer is repainted
  (dirty-rectangle, not full-frame).

**Marginal (do carefully, with decimation):**
- Backplotting a **very large** program (tens of thousands of segments) in one
  pass: fine as a one-time parse, but **don't redraw the whole path every
  frame**. Pre-rasterize the static path to an off-screen buffer once, then only
  composite the moving marker + "already-cut" overlay on top. Decimate segments
  below ~1px when zoomed out.
- Smooth continuous scrolling/animation: keep to a few FPS and dirty-rects;
  don't chase 60 Hz.

**Too much (don't promise it):**
- **3D solid / stock-removal simulation** — real-time voxel or Z-buffer
  rendering of material removal is out on a GPU-less 454 MHz ARMv5. At most a
  static isometric **wireframe** could be drawn, and even that redrawn only on
  demand, not animated. Treat 3D as **not available on-device** (it can live in
  the optional desktop/browser client that talks to the same backend instead —
  see [architecture.md](architecture.md); the desktop has the horsepower, the
  panel does not).
- Alpha-blended gradients, drop shadows, anti-aliased thick strokes everywhere —
  emWin can, but it costs; keep the look **flat and high-contrast** (which also
  reads better on a workshop LCD).

**Net:** the on-device UI should look like a sharpened, better-organized version
of the vendor screen — flat 2D, big legible DRO, a real 2D toolpath — and leave
anything 3D/heavy to the optional off-device client.

## Input model — physical keys (all mapped, [ui-controls-map.md](ui-controls-map.md))

The panel has **18 keys**, already mapped to TM1638 scan codes live. Group them
by the role the new UI gives them. Key principle: the **bottom row is
silkscreen-labelled** (Start/Pause/Reset/View/TryCut/Spindle/PageR), so their
meaning is largely fixed — the UI shows a soft-label **above each one** that
refines the action for the current screen, but never contradicts the printed
label.

### Bottom function row (F1–F7) — primary actions

| Key | Role |
|---|---|
| **Start** (F1) | Begin/resume the armed stage (Simulate / Dry-run / Run), per the current arming |
| **Pause** (F2) | Feed hold (resumable) |
| **Reset** (F3) | Stop + clear alarm; returns to idle |
| **View** (F4) | Cycle the centre view: DRO ↔ Toolpath ↔ Program listing ↔ Diagnostics |
| **TryCut** (F5) | Toggle dry-run arming (spindle suppressed + Z clamped) for the next Start — reuses the vendor's own TryCut key for exactly the TryCut concept in [workflow-and-resume.md](workflow-and-resume.md) |
| **Spindle** (F6) | Manual spindle on/off (setup) |
| **Page R** (F7) | Page rotate — the vendor's own F7 cycles Main → File → Param (msg-eng #207-227); ours: Run → Files → Work → Tools → Settings. Current key model: [operator-procedures.md](operator-procedures.md) |

### Right-hand nav/edit cluster

| Key | Role |
|---|---|
| **Mode** | Switch top-level mode: Auto / Manual (jog) / MDI |
| **Tab** | Move focus between panels/fields on the current screen |
| **Select A** | Select/activate the focused item (pick a file, pick a G-code line to resume from) |
| **Shift** | Modifier (coarse jog, reverse View cycle, etc.) |
| **Modify/Probe** | Edit focused value / start a probe routine |
| **Enter/Home** | Confirm / "go home" |
| **Cancel / Z+** | Cancel / back (doubles as Z+ jog in Manual mode) |

### Arrow pad — dual purpose by mode

- **Manual mode:** `X−/X+`, `Y+/Y−` (the Y rocker), `Z+` (Cancel), `Z−`
  (Enter/Home) jog the respective axis.
- **Menu/Auto mode:** `Y+/Y−` = move cursor up/down a list; `X−/X+` = left/right
  / adjust value; `Z±` = page up/down. This is how a G-code line gets selected
  for resume without a touchscreen.

**Safety requirement (user feedback, 2026-10-18):** the stock legends give one key up
to three meanings (e.g. `Z− / ENTER / HOME`) with no visible cue for which one applies,
so "confirm" or "home" can turn into jogging Z down. In the new UI:

- **Motion only in an explicit, clearly indicated Jog mode.** Outside it, the arrow keys
  never move an axis.
- **Enter never causes motion.** Homing has its own on-screen confirm step, never an
  implicit `ENTER/HOME` action.
- Shift + lowercase `x`/`y`/`z` legends are not needed for jogging. Any second-layer
  function must be shown on screen while Shift is held.

### Open input item

`MOTION_IOC_KEY` through the vendor driver still reports **zero** key events
([ui-controls-map.md](ui-controls-map.md)) even though the TM1638 hardware scan
works directly. The new app should **read the keypad the way the probe tools
proved works** — the direct TM1638 scan in `driver/motiondev_lld.c` /
`mockup/ioctl-probe/tm1638read.c` — rather than depend on the (currently broken)
`MOTION_IOC_KEY` ioctl path, or that ioctl must be fixed first. Flag for the
driver side; it doesn't change the UI design, only which syscall the backend
uses to get key events.

## Input model — the MPG pendant (repurposing axis positions 5 & 6)

> **Superseded 2026-10-17.** [mpg-pendant-io.md](mpg-pendant-io.md) later showed that
> positions **OFF, 5 and 6 have no wires** and read identically, so the 5-vs-6 split
> below can't be detected. The working mapping uses "OFF/5/6 + dead-man held" with the
> **multiplier** as the function selector (X1 = line scrub / feed %, X10 = 10-line scrub /
> spindle %, X100 = section jump / rapid %). See [operator-ux.md](operator-ux.md)
> § Pendant mapping. The original proposal is kept below for the record.

The pendant (model MBL-100P-5L, 100 ppr) has a **wheel** + an **axis-select
rotary** (`OFF, X, Y, Z, 4, 5, 6`) + a **multiplier** (`X1, X10, X100`) + an
**E-stop** ([connector-pinouts.md](connector-pinouts.md)). In the companion
peripheral-mapping work **only the wheel is mapped so far**; this design assumes
the axis-select switch positions become readable (they're on the same 15-pin
MPG connector, mappable the same way the wheel was). The wheel count itself is
`FPGA_REG_MPG_COUNT = 0x0E` ([mpg-investigation.md](mpg-investigation.md)).

A hobby machine is ≤4 axes, so switch positions **5 and 6 address axes that
don't physically exist** — free to repurpose as "what does the wheel do"
selectors. Proposed mapping (the wheel is a relative encoder, so it always means
"nudge the currently-selected thing"):

| Switch | While **idle / program loaded** | While **running a program** |
|---|---|---|
| **OFF** | Wheel inert (safe default) | Wheel inert |
| **X / Y / Z** | Jog that axis; multiplier X1/X10/X100 = fine/med/coarse step | (jog disabled during run) |
| **4** | Jog A axis if configured, else inert | inert |
| **5** | **Scrub the G-code line cursor** (select resume point); mult = 1 / 10 / 100 lines per detent | **Feed override (FRO %)** — trim 0–200% live |
| **6** | **Zoom the toolpath view** (or adjust a focused setpoint); mult = zoom rate | **Spindle override (SRO %)** — trim live |

This gives the operator, *mid-cut*, a physical feed-% wheel (switch to 5) and a
physical spindle-% wheel (switch to 6) — exactly the "control movement speed %
and mill rotational speed from the pendant" the user asked for — and, when
idle, turns the same wheel into a G-code line scrubber for picking a resume
point by hand (position 5), which pairs directly with the resume flow in
[workflow-and-resume.md](workflow-and-resume.md). OFF is the safe parking
position. Positions 5/6 are the only repurposed ones; X/Y/Z/4 keep their natural
jog meaning.

> Other practical uses for 5/6 worth considering later: a "MDI value" wheel when
> editing a field (5), a WCS/work-offset selector (6 while idle), or a jog-speed
> trim. The run-time Feed/Spindle-% roles above are the most valuable and are
> what the mockup shows.
