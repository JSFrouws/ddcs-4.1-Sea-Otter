# FPGA outputs — OUT1-3 and axis DIR — CONFIRMED

_2026-10-16. Found by tracing the real app's register writes, then proven by writing the
registers ourselves (app stopped) while the user measured the pins with a multimeter.
Desk unit with COM+/COM- fed from the 24 V supply ([hardware-setup.md](hardware-setup.md)).
Raw trace: [raw/2026-10-16_fpga-outputs/iotrace_run3.log](raw/2026-10-16_fpga-outputs/iotrace_run3.log)._

## Answer

| Function | FPGA register | Encoding | Confirmed by |
|---|---|---|---|
| **OUT1** (pin 21) | `0x3E` bit 0 | 1 = on (~24 V, measured COM+ → OUT) | app trace (M3) + our own write |
| **OUT2** (pin 3) | `0x3E` bit 1 | 1 = on | app trace (M8) + our own write |
| **OUT3** (pin 22) | `0x3E` bit 2 | 1 = on | our own write (no M-code mapped to it in the trace) |
| Spindle speed (analog) | ioctl `0x4627` (SoC PWM6, not FPGA) → **VSO (pin 2)** | duty 0..3000 → 0..10 V linear (measured 0.03 / 5 / 10 V) | app trace + own ioctl + meter |
| **Axis DIR** (`XD±`, ...) | `0x04` bit 1, in the jog/move command | `3` → XD+ − XD- = **−3.5 V**, `1` → **+3.5 V** | app trace (MPG jog) + our own write |

`0x3E` is a plain write-only bitmask: write the whole mask each time (the app does).
Writing it switches the opto/transistor outputs straight away, with no latch or strobe.

The app's mapping (M3 → port 1, M8 → port 2, M10 → port 3, M4 → port 0 = unused) is
purely software: it only decides which `0x3E` bit to set. The FPGA doesn't know about
M-codes.

## Axis select and DIR per axis (all 4 confirmed, 2026-10-16)

Three static rounds (app stopped, a 1-step command per axis with different DIR patterns,
user measuring `xD+` − `xD-` with red on +) pinned down both the axis numbering and the
polarity:

| `0x00` select | Axis | Pins (D+ / D-) | Command `3` | Command `1` |
|---|---|---|---|---|
| 0 | **X** | 30 / 12 | −3.5 V | +3.5 V |
| 1 | **Y** | 32 / 14 | +3.6 V | −3.6 V |
| 2 | **Z** | 34 / 16 | +3.6 V | −3.6 V |
| 3 | **A** | 36 / 18 | +3.6 V | −3.6 V |

| Round | Commands for select 0/1/2/3 | Measured X / Y / Z / A |
|---|---|---|
| 1 | 1 / 3 / 1 / 3 | 0 V (probe contact) / + / − / + |
| 2 | 1 / 1 / 1 / 1 | 0 V (probe contact) / − / − / − |
| 3 | 3 / 3 / 1 / 1 | − / + / − / − |

**X's polarity is inverted compared with Y/Z/A.** Our writes were identical for every
axis, so the inversion must sit in per-axis FPGA config that the app loads at boot. Most
likely that's the app's "dir electric level" setting, which the user toggled for X
earlier in the session and which may not match the other axes. That inversion is `0x02` bit 1 (see below).

## Per-axis electrical config `0x02` — pulse polarity and DIR invert (both CONFIRMED)

Every time a setting is saved, the app rewrites a whole config block, including a banked
per-axis word at **`0x02`**, written as `0x00 = <axis>`, `0x02 = <value>`, `0x00 = 0x0F`.
Trace: [raw/2026-10-16_fpga-outputs/iotrace_pol.log](raw/2026-10-16_fpga-outputs/iotrace_pol.log).

| Bit | Meaning | Status |
|---|---|---|
| 2 | **Pulse polarity**: 1 = idle `xP+` − `xP-` = **+3.5 V** ("high"), 0 = **−3.5 V** ("low") | **Confirmed**: app toggle on Y gave +3.5 / −3.5 / +3.5; our own write (X=6, Y=0, Z=4, A=0) gave XP +, YP −, ZP +, AP − exactly |
| 1 | **DIR invert**: 1 inverts the axis's DIR output | **Confirmed**: our own write X=4, Y=6, Z=4, A=6 with command `1` on every axis gave XD −, YD +, ZD − (control), AD +, i.e. X/Y/A flipped and Z unchanged |

Values the app writes on this unit (its saved settings): X = `6` (X dir level was changed in the app), Y/Z/A = `4`.
With bit 1 = 0: DIR command `3` → +3.6 V, `1` → −3.6 V (Y/Z/A measured); bit 1 = 1 reverses that (X).

Why the inversion lives in the FPGA: the FPGA generates the pulse train itself (µs pulses,
~100 kHz), and the app only issues "N steps, direction". So pulse polarity *has* to be set
where the pulses are made. DIR invert could have been done in software, but keeping
it next to the pulse invert gives the motion code one sign convention and lets the FPGA
keep the DIR setup time relative to the first pulse.

## Pulse outputs (all four confirmed via polarity)

| Axis | Pins (P+ / P-) | Idle with `0x02` bit 2 = 1 |
|---|---|---|
| X | 29 / 11 | +3.5 V |
| Y | 31 / 13 | +3.5 V |
| Z | 33 / 15 | +3.5 V |
| A | 35 / 17 | +3.5 V |

A long move (Y, ~1M steps, app stopped) set read register `0x01` bit 9 (busy) for ~10 s.
On a DC meter that only showed as a 0.1 V dip on YP, so the pulses are short and low
duty. A meter can't usefully see pulses. Use the static polarity test or a scope.

**Resolved 2026-10-18 ([fpga-motion.md](fpga-motion.md)):** there is no done latch - `0x67` is a motion watchdog, and without it moves are accepted but never stepped. The original note follows. **Open: move "done" acknowledge.** After a finished move, read `0x00` shows `0x0082`
(Y bit 1 + bit 7), and new moves are ignored until the app re-initialises the FPGA.
Writing `0x04 = 4 → 0`, `0x32 = 1 → 0` or `0x2E = 0` did **not** clear it. The ack
register is still unknown. The app's init block (look for `0x41 = 1 → 0`, `0x35`, `0x34`
writes) is the next place to look.

## Jog / move command sequence (as the app sends it for an MPG jog)

```
0x00 = <axis>        axis select: 0 = X (the app cycles 0..3 constantly; banked regs)
0x0A = <steps lo>    step count, low 16 bits   (app used 0x32..0x1F4 = 50..500 per MPG tick burst)
0x0B = <steps hi>    step count, high 16 bits
0x03 = 2             unknown - always 2 for jogs (mode/rate profile?)
0x04 = 1 or 3        command: bit 0 = go/strobe, bit 1 = direction
0x04 = 0             release the command
```

Our proof writes (`motionw`, app stopped, X axis, 1 step so nothing meaningful moves):

```
motionw 0x3e=1 0x00=0 0x0a=1 0x0b=0 0x03=2 0x04=3 0x04=0   -> OUT1 on, OUT2/3 off, X DIR -3.5 V
motionw 0x3e=6 0x00=0 0x0a=1 0x0b=0 0x03=2 0x04=1 0x04=0   -> OUT1 off, OUT2/3 on, X DIR +3.5 V
```

The DIR level stays latched after the move. The **sign** is as measured with the app's
"X dir electric level" setting on **high**. That setting probably just makes the app
send the opposite bit (not verified at register level). DIR is a differential
line-driver output (±3.5 V between `XD+` and `XD-`), not open-collector.

Other writes seen around motion, not decoded yet: `0x04 = 4 → 0` pulses (stop/clear?),
`0x32 = 1 → 0` pulses, `0x2E = 0` after each jog, `0x0E/0x0F` and `0x23/0x24` streams
(32-bit values, probably per-axis speed/acceleration), and `0x67 = 0` every ~63 ms (a
heartbeat; stopping it for minutes has no visible effect). Note that **write and read
registers differ**: e.g. writing `0x0E` isn't the MPG count you read at `0x0E`.

## Tools

- [../mockup/ioctl-probe/iotrace.c](../mockup/ioctl-probe/iotrace.c): `LD_PRELOAD` shim
  for `ddcsv4.out` (dynamically linked, glibc 2.13). It logs every `/dev/motion` write,
  block and PWM call to `/tmp/iotrace.log` and passes all ioctls through unchanged. Start:
  `killall ddcsv4.out; cd /ddcsv4 && LD_PRELOAD=/ddcsv4/iotrace.so ./ddcsv4.out &`.
  Build notes are in the file header (needs `-D_TIME_BITS=32` for the old glibc).
- [../mockup/ioctl-probe/motionw.c](../mockup/ioctl-probe/motionw.c): replays
  `addr=value` writes (and `r<addr>` reads) through the vendor driver. App must be
  stopped (one opener only).

## Gotchas hit

- **Per-address de-duplication in a trace hides real writes**: many registers are
  banked per axis via `0x00`, so the same address legitimately carries a different value
  per axis. The first trace version de-duplicated and missed most of the settings
  writes. Log everything instead (the idle app is quiet apart from `0x67`).
- `/tmp` is a RAM disk: a reboot or power-cycle wipes trace logs. Copy them off promptly.
- The app's restart (screen blanks for a few seconds) is not a board reboot. One real
  reboot happened during this session with no software cause found (no watchdog armed,
  no reboot call in the app). Most likely a power dip while probing the 37-pin connector.
