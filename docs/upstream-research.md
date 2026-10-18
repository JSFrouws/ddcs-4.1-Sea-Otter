# Upstream Pandora-CNC: did they already build a new UI / simulator / toolpath viewer?

_2026-10-15. Web search plus a direct look at every repo under the
[Pandora-CNC GitHub org](https://github.com/Pandora-CNC) (12 repos, mostly
archived Jan 2023). No non-GitHub forum/wiki was found for this project —
search for "Pandora-CNC" turns up almost nothing outside this org._

## Short answer

**No.** Nothing in the org amounts to a G-code simulator, a live
position/toolpath display, or a finished UI replacement. There is exactly
one relevant repo, **`pdui`**, and it is a bare SDL drawing-primitives
skeleton (background/blit/text helpers, ~4 source files), not a working
screen or any simulation/visualization code. No issues, pull requests, or
README text anywhere in the org mention simulation, toolpath preview, or
position visualization as a stated plan.

## What the org actually contains

| Repo | What it is | Relevant to UI/sim? |
|---|---|---|
| [`motiondev`](https://github.com/Pandora-CNC/motiondev) | DDCSV1.1 motion driver rewrite, **not archived** (the only active repo) | No — but this is the direct ancestor of our driver's trace/bypass idea, see below |
| [`pdui`](https://github.com/Pandora-CNC/pdui) | "User interface for the Pandora CNC system" (C++/SDL), archived | The only UI-shaped repo in the org — see below |
| [`pdmotiond`](https://github.com/Pandora-CNC/pdmotiond) | Motion daemon, Unix-socket IPC to a client app, archived | Infrastructure a UI would sit behind, not a UI itself |
| [`motionutil`](https://github.com/Pandora-CNC/motionutil) | CLI for controlling FPGA registers directly, archived | Diagnostic tool, like our `mtrace` |
| [`pdkeypad`](https://github.com/Pandora-CNC/pdkeypad) | Keypad driver, archived | No |
| [`pdfbtext`](https://github.com/Pandora-CNC/pdfbtext) | "Pandora CNC framebuffer text driver", archived | Text-only framebuffer output, not graphical |
| [`Pandora-Reverse-Engineering`](https://github.com/Pandora-CNC/Pandora-Reverse-Engineering) | Disassembly notes for the DDCSV1.1 stock app (`application.asm`/`.c`) | Same kind of effort as our own `docs/app-analysis.md`, not UI/sim |
| `PandoraInstaller`, `PandoraLoader`, `PandoraImager`, `PandoraBuild` | Installer, bootloader, NAND imaging, build system | No |

### `pdui` — the one UI attempt

`pdui`'s README is a single line ("User interface for the Pandora CNC
system") plus a font credit (Bitstream Vera). Its `src/` directory has four
files: `main.cpp`, `params.cpp`, `styles.cpp`, `ui.cpp`. Reading `ui.cpp`
(78 lines) directly: it links `SDL`, `SDL_image`, `SDL_ttf`, and defines only
primitive helpers — `ui_init`, `ui_clear`, `ui_blit`, `ui_draw_str_solid`,
pixel-color conversions. There is no canvas/scene code, no coordinate
transform for machine position, no G-code parser, no path/line drawing
beyond generic blits. It reads as an early UI-toolkit skeleton that never
got far enough to display anything CNC-specific, let alone simulate motion
or draw a toolpath. It has 0 stars, 0 forks, 1 watcher, and no activity
beyond the initial commits before the Jan 2023 archive.

### `motiondev` (the V1.1 ancestor of our driver) — confirms the trace/bypass lineage

The still-active `motiondev` repo's README documents exactly the trace +
read-bypass + register-filter model our `/dev/motion-trace` is built from:
a `control` userspace tool with `r`/`e`/`d` (reset/enable/disable trace),
`sX_Y` / `cX` (set/clear read-bypass at a register), `f(w/r)X_Y` /
`p(w/r)X_Y` (block/pass trace filters by register range and direction), `g`
(dump unread log), and raw `RX` / `WX_Y` register read/write — all against
a configurable `TRACE_SIZE` (250000 records there vs. our 65536) and
`NUM_OF_REGS` (167 there vs. our 256, since the V4.1 FPGA address space is
a full byte). This is the "Pandora's idea" the README and
[driver-comparison.md](driver-comparison.md) §4 refer to: our port keeps the
same enable/disable/bypass/filter model and extends its *event* coverage
(MPG, keypad, LEDs, spindle, power-fail) beyond what the V1.1 tool's README
describes, which only talks about raw register trace/bypass, not
synthesized peripheral events.

## What this means for "should we build a mockup"

Upstream has not solved, or even seriously attempted, the "something for a
new UI to render against" problem — there is no simulator, no position
visualizer, no toolpath preview anywhere in the Pandora-CNC lineage to reuse
or diverge from. See [../mockup/README.md](../mockup/README.md) for the
recommendation and the small prototype built as a result.
