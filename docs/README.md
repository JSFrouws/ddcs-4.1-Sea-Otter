# docs/ — DDCS V4.1 findings

Everything learned about the real unit goes here: notes as Markdown, and raw
material (boot logs, scans, dumps, captures) in its native format under `raw/`.

| File | What |
|---|---|
| [user-manual.md](user-manual.md) | **User manual** for the operator: screens, keys, daily procedure, E-stop/limit behaviour (controlled stop), macros, settings, pendant |
| [usb-install.md](usb-install.md) | **Install Sea Otter from a USB stick** (start script, network, SSH, switching back to the vendor app) |
| [ssh-setup.md](ssh-setup.md) | **SSH with a user name + password**: USB route, manual route, building dropbear with password login |
| [api-reference.md](api-reference.md) | **The implemented HTTP API**, endpoint by endpoint, incl. all settings keys |
| [io-map.md](io-map.md) | **Start here for I/O**: every connector pin and panel control → SoC GPIO / FPGA register, with status |
| [hardware-setup.md](hardware-setup.md) | How the unit is wired on the desk for tests |
| [network.md](network.md) | Ethernet access: IP settings, port scan results, login attempts |
| [smb-shares.md](smb-shares.md) | The unit's Samba shares (`SYSDISK` = app folder, `CNCDISK` = G-code) |
| [motiondev-ko.md](motiondev-ko.md) | Unit's stock driver vs. our register map + the modversions CRC fix |
| [app-analysis.md](app-analysis.md) | ddcsv4.out: driver load, filesystem layout, system() calls, routes to a shell |
| [cifs-handshake.md](cifs-handshake.md) | Live capture of the unit as CIFS client: root, empty password (proven), SMB1/NTLMv1, kernel string |
| [driver-comparison.md](driver-comparison.md) | Our `driver/motiondev.c` port vs. the vendor's `motiondev.ko`: ioctl ABI, GPIO/FPGA handling, locking, and what's confirmed vs. still an assumption |
| [upstream-research.md](upstream-research.md) | Did Pandora-CNC upstream ever build a UI/simulator/toolpath viewer? (No — see `../mockup/`) |
| [shell-access-recon.md](shell-access-recon.md) | Static firmware recon for a shell path: no vendor debug/service backdoor exists; full file/parameter menu; options weighed |
| [shell-access-live-test.md](shell-access-live-test.md) | The `ddcsv4.out` swap: risk reasoning, what `recovery_shell` does, the live procedure and rollback |
| [rootfs-findings.md](rootfs-findings.md) | Live-shell findings: boot script (**the real USB firmware-update path**), driver load, USB hotplug handling |
| [ssh-access.md](ssh-access.md) | Persistent SSH (dropbear, static ARM build, public-key auth), wired into `rcS` |
| [mpg-investigation.md](mpg-investigation.md) | **RESOLVED**: live MPG count is FPGA register `0x0E` (confirmed live on hardware), not the SoC's TIMROT or the assumed ioctl - driver updated |
| [ui-controls-map.md](ui-controls-map.md) | All 18 buttons + all 7 LEDs mapped to TM1638 codes, confirmed live on hardware. `MOTION_IOC_KEY` still broken/open |
| [connector-pinouts.md](connector-pinouts.md) | Full silkscreen pinouts: 37-pin axis/IO connector, HMI/RS232 (unused), and the dedicated 15-pin MPG port (the pendant's real port) |
| [mpg-pendant-io.md](mpg-pendant-io.md) | **RESOLVED**: pendant axis/X1-X10-X100/E-stop = opto inputs in FPGA reg `0x3D` (bit map), dead-man gates them, wheel `0x0E` ungated. Needs COM+/COM- fed |
| [fpga-outputs.md](fpga-outputs.md) | **CONFIRMED by multimeter**: OUT1-3 = FPGA reg `0x3E` bits 0-2; axis select `0x00` 0-3 = X/Y/Z/A; DIR = bit 1 of move command `0x04`; pulse polarity = per-axis `0x02` bit 2; all 8 axis pins measured; jog sequence; `iotrace.so` app-trace shim + `motionw` writer |
| [machine-settings.md](machine-settings.md) | The real garage CNC's saved `setting` file, fully decoded (steps/mm, travel/soft limits, home config, shared limit+home wiring, spindle, network) — now the replacement app's defaults |
| [ui-requirements.md](ui-requirements.md) | Structured requirements for a replacement operator UI (from Eding/LinuxCNC/PlanetCNC/Mach4/GRBL-sender research) |
| [workflow-and-resume.md](workflow-and-resume.md) | Select → simulate → dry-run (TryCut) → run pipeline, live visualization, and the "remember work done" + resume-from-line strategy |
| [architecture.md](architecture.md) | Proposed build: reuse driver + mockup, `MotionBackend` abstraction, new G-code interpreter, resume log |
| [operator-procedures.md](operator-procedures.md) | **Current UI design**: what Page R is (vendor msg file), key model (Mode/Page R/Tab/Shift), the machine state machine (no jog while running), digit editor, decimals, step-by-step operator procedures incl. multi-part zeroing and an AI agent driving a job over the API, rendering choice |
| [fpga-motion.md](fpga-motion.md) | **Measured on the unit**: `0x67` = motion watchdog + position latch, position counters `0x02–0x09`, per-axis status `0x01`, rate words (top rate = 2×A, B = FPGA ramp), 40 000 steps/s ceiling, `0x04=4` = stop (holds the axis), bursts only into idle axes; app made step-exact (0 faults) |
| [app-architecture.md](app-architecture.md) | **The device app (`../app/`)**: C, reuses the vendor `motiondev.ko` via ioctl + TM1638 via `/dev/mem`, software interpolation with step bursts, single-threaded loop, register-level FPGA simulator, layer tests incl. write-for-write comparison with the vendor app's trace; open items before the first real cut |
| [motion-planner.md](motion-planner.md) | The lookahead planner: what a segment is, depth by count and braking distance, per-axis corner jump, backward/forward passes, S-curve with per-axis jerk, the applied-feed bar, tuning |
| [ui-hardware-constraints.md](ui-hardware-constraints.md) | Real-panel limits: **confirmed over SSH** 1024×600 16bpp (RGB565) framebuffer, no touch, software-2D graphics budget, physical-key + pendant input model |
| `raw/` | Unedited tool output (scans, logs, dumps), named `YYYY-MM-DD_<what>.<ext>` |

**⚠ Read [shell-access-live-test.md](shell-access-live-test.md) first if picking this up fresh**
(e.g. next session) - it has the end-of-session status box: the real app is restored but
needs one power-cycle to take effect, and exactly what to check afterward.

Background on the driver port and the register map: [../README.md](../README.md)
and [../driver/motiondev_hw.h](../driver/motiondev_hw.h). A mockup `/dev/motion`
server for developing a future UI/automation app without hardware is in
[../mockup/](../mockup/README.md).
