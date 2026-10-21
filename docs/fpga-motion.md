# FPGA motion: what the hardware really does (measured 2026-10-18)

_On the desk unit, no motors connected, vendor app stopped. Tools: `app/tools/fpgaprobe.c`
(scripted register experiments through the vendor `/dev/motion`), `app/tools/iotrace2.c`
(LD_PRELOAD shim on `ddcsv4.out` that also logs reads and can **spoof the pendant**), and the
replacement app itself with `PANDORA_TRACE`. Raw data: [raw/2026-10-18_motion-probe/](raw/2026-10-18_motion-probe/).
Supersedes the "done latch" open item in [fpga-outputs.md](fpga-outputs.md)._

## Registers found

| Register | Finding |
|---|---|
| **W `0x67`** | **Motion watchdog + position latch.** A burst is accepted but never stepped unless `0x67` is written regularly; a 300 ms gap only *pauses* the move (it resumes). Each write latches the position counters. This - not a done latch - is why "moves were ignored after the first" in the earlier session. |
| **R `0x02`–`0x09`** | **Position counters**, 32-bit steps: X `03:02`, Y `05:04`, Z `07:06`, A `09:08` (hi:lo). Matched the vendor DRO exactly (Y 548 982 = 274.491 mm, A −2000 = −1.000). Count in the DIR-bit sense, survive app restarts, reset only by power. |
| **W `0x0C`/`0x0D` + `0x05`** | Position preset: select axis, 32-bit value, `0x05` 1→0. The vendor restores its saved positions this way at boot. |
| **R `0x01`** | **Per axis**: bit *n* = move active, bit 8+*n* = stepping. (The earlier "bit 9 = busy" was just axis Y.) |
| W `0x04` | 1/3 = go (DIR clear/set; **set = counter counts up**, confirmed), **4 = stop**: decelerates within ~20 ms, but mid-move it leaves the axis *held* (bit *n* stays set, all further bursts ignored) until **power-off** - no release found among ~40 candidates, and the vendor app never stops mid-move. At rest, 4 is harmless. |
| W `0x12/0x13` (A), `0x14/0x15` (B) | Burst rate words. **Top rate = 2 × A steps/s** (8 072 vs 8 192, 15 310 vs 16 384 measured). **B scales the FPGA's own acceleration ramp**: 400 steps at 50 k/s take 80 ms with B = `0x9C40`, 8 ms with B = `0x1000000`. |
| W `0x06`–`0x09` (banked) | FPGA soft limits, 32-bit (vendor writes this machine's 10…915 / 10…590 / −300…−10 mm). |
| W `0x10`–`0x1D`, `0x0E/0F`, `0x23`–`0x26`, `0x2B/2C` | Per-axis motion parameters / clock constants from the vendor init (20 000 000 = 20 MHz clock?). Replayed unchanged by the app (`src/hal/fpga_vendor_init.c`). |

## Burst behaviour

| Situation | Result |
|---|---|
| Burst into an idle axis | **Step-exact** in every test (hundreds of bursts, any rate, any size from 1 step, both directions, multi-axis, with keypad scanning, with input reads, with stalls) |
| Second burst, same direction, while stepping | Usually appended; **under the app's real timing some steps were lost** (X short by 1 200–3 900 steps per zig-zag run). Not reproducible in isolation - timing dependent. |
| Second burst, opposite direction, while stepping | **Ignored** (the steps are lost) |
| Fastest step rate | **~40 000 steps/s on every axis** whatever A says (= 1200 mm/min at 2000 steps/mm, the vendor's own max manual speed) |
| Start overhead | ~2 ms per burst on top of steps/rate |
| No heartbeat | accepted, not stepped; resumes when the heartbeat returns |

## Consequences for the app (implemented in `app/`)

1. **Bursts only go into an idle axis** (`0x01` bits *n* and 8+*n* clear); a busy axis keeps its steps for the next slice.
2. Rate = steps ÷ (0.85·slice − 2 ms), capped at 40 000 steps/s; B = `0x1000000` so the **planner** does all the ramping and the axes stay synchronised.
3. Main loop: one tick with the real elapsed time, clamped to 10…40 ms (rendering stalls the loop ~50 ms on this CPU).
4. Heartbeat every 20 ms; the counters are compared with the commanded steps whenever everything has been at rest for 3 heartbeats (and re-synced to the counter if they ever differ).
5. "Done" (program end, goto-zero) waits until the FPGA has finished.
6. Reset / E-stop / limit drop every planned-but-unsent step: the machine stops at the end of the burst in flight. **Never `0x04 = 4` mid-move.**
7. Homing presets the counter; start-up takes the machine position from the counters.

## Results with the final build (the Sea Otter app running on the unit, real FPGA)

| Test (`app/tools/device_motion_test.sh`, `device_stop_test.sh`) | Counters vs commanded |
|---|---|
| Jog X +10 mm | exact |
| 20 alternating ±1 mm jogs sent at once | exact, nets to 0 |
| Bracket_v3.nc (pocket, profile arcs, 3 drills, M6 pause) | exact |
| Sign_rosette.nc (1 200 short segments, constant reversals) | exact |
| Pause, resume, Reset mid-run | exact |
| 40 pendant-style 0.005 mm nudges | exact |
| Position checks over the whole campaign | **13 checks, 0 faults** |
| Overrun after Reset at 20 mm/s (measured on the unit) | **0.0–1.0 mm** (was 30 mm before the fixes) |
| Overrun after Reset at 16 mm/s, local-66 (2026-10-18), abrupt stop | **0.24–0.51 mm** |
| Overrun after Reset at 16 mm/s, local-66, controlled stop (default) | **0.88–0.97 mm**, 0 faults |
| propellor_bolkop.nc (Fusion 3D, 4 928 lines), real run, 300 % feed | 1 671 lines / ~10 min, **0 ms lost, counters exact** |
| propellor_8mm_en_bolkop.nc (Fusion, 64 396 lines, G83 drilling), real run | 1 299 lines / 761 s motion, **0 ms lost, counters exact** |

### 2026-10-18: the planned-but-unsent backlog

Before local-66 a stop overshot 2.0 mm (abrupt) and 3.1–3.5 mm (controlled). Cause (found
by the planner session): the plan ran at the full 40 000 steps/s, but idle-only bursts with
~2 ms start overhead can't sustain that, so planned steps piled up unsent and every stop
still sent the pile. Fix: planned axis speed capped at **80 % of the FPGA ceiling** (16 mm/s
= 960 mm/min at 2000 steps/mm), feed × override never above that cap (300 % can't plan past
the hardware), one burst carries at most 30 ms. The honest sustained top speed is therefore
960 mm/min, not 1200.

## Still open

- **Real motors**: no motor was connected. Bursts are stop-go (the axis idles ~15 % of each 10 ms slice), which a stepper at speed may not like - check with a motor, or find a safe append.
- **The 40 000 steps/s ceiling** - not in `0x16/17`, `0x2B/2C`; probably another init parameter or the pulse generator itself.
- **Release of a mid-move stop** - unknown; avoided.
- The vendor's coordinated block interface (`0x4622` → `0x50…0x60`) would give true interpolation and continuous motion; still not decoded.
- Homing on real switches - not tested (nothing wired on the desk unit).
