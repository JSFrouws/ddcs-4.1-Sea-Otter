# Hardware setup

Sea Otter has been developed and tested on two setups: a bare controller on a bench
(2026-10-15 to 2026-10-31), then the same controller on a three-axis stepper machine
(since 2026-10-31).

## On a machine

The unit drives a three-axis machine, with the drives and the limit/home switches
connected.

- **Limit/home switches:** NC, one per axis, on **IN02 / IN03 / IN04** (X / Y / Z); closed at
  rest (`0x37` = `0xFFF1`), `limits.level_high` = 1. They are **not** handed to the FPGA's own
  limit function (it treats closed as active and froze every axis): limits and homing are in
  software only, see [fpga-motion.md](fpga-motion.md) 2026-10-31.
- **E-stop:** IN01, `estop.level_high` = 0. **Drive alarms:** IN10–IN13.
- **Drives:** stepper drives at **2000 pulses/rev** on 5 mm pitch spindles, so
  **400 steps/mm** on X, Y and Z. NEMA23 motors (200 full steps/rev).
- **A dead DIR output is worked around by remapping:** this unit's X DIR output swings
  −3 V / +0.77 V instead of ±3 V (also with nothing connected), so the X drive saw one
  direction only. The **X drive is wired to the A connector** (`AP±`/`AD±`):
  `axis.X.output` = `A`, `axis.A.output` = `-`. X keeps its own `dir_invert` (it belongs to the
  drive's wiring). Any axis can be moved to a spare connector this way.
- **Switch overtravel vs. braking:** limit stops and the E-stop brake on the ramp
  (`motion.graceful_stop`), and braking is not blocked by a tripped switch. Travel past the
  switch point = v·t_tick (the switch is polled once per tick, up to 34 ms seen) + v·t_queue
  (the blocks already queued in the FPGA, `motion.queue_ms`, 50 ms) + v²/2A + vA/2J, at the
  highest speed the axis reaches (`max_vel`, also G0). Example with 20 mm/s:

  | axis | max_vel | accel / jerk | detection | in flight | braking | **total** |
  |---|---|---|---|---|---|---|
  | X | 1200 mm/min (20 mm/s) | 300 / **100** | 0.68 mm | 0.60 mm | ≈8.9 mm (jerk-limited) | **≈10 mm** |
  | Y | 1200 mm/min (20 mm/s) | 300 / 10000 | 0.68 mm | 0.60 mm | 0.67 + 0.30 mm | **≈2.3 mm** |
  | Z | 1200 mm/min (20 mm/s) | 300 / 10000 | 0.68 mm | 0.60 mm | 0.67 + 0.30 mm | **≈2.3 mm** |

  With a low `jerk` (100 here on X), braking below v = A²/J (900 mm/s) is jerk-limited:
  t = 2·√(v/J), d ≈ v·t/2 - 8.9 mm from 20 mm/s, 1.1 mm from 5 mm/s. With `jerk` 10000 X
  would be back to ≈2.3 mm like Y and Z.

  Every switch needs more overtravel than that (check by hand before relying on it).
  Raising `max_vel` grows it fast: 40 mm/s gives ≈6.5 mm.
- **Network:** static `192.168.1.200` as before (see [network.md](network.md)).

## Bench setup

The controller alone on a bench, no machine attached.

- **Power:** 24V bench adapter into **pin 19 (24V)** and **pin 37 (GND)** of the large main
  (37-pin) connector, plus **COM+ (pin 20) = +24V and COM- (pin 1) = 0V** from the same
  supply - required for the opto inputs (pendant switches, E-stop); without it the app sits in
  permanent E-stop. Full pinout in [connector-pinouts.md](connector-pinouts.md).
- **Attached:** only the MPG pendant (MBL-100P-5L), plugged into the **dedicated 15-pin
  "MPG" port** (not the HMI/RS232 port; see [connector-pinouts.md](connector-pinouts.md)).
  Wheel = FPGA `0x0E`, switches = FPGA `0x3D` - see [mpg-pendant-io.md](mpg-pendant-io.md).
- **Not attached:** stepper/servo drivers, spindle, limit switches, the 37-pin connector's
  I/O. Nothing can move, and none of the 37-pin connector's inputs/outputs will show any
  activity.
- **Ethernet:** the unit has an RJ45 port, static IP `192.168.1.200/24` (see [network.md](network.md)).
- **Shell access:** persistent root SSH via dropbear, see [ssh-access.md](ssh-access.md).
  UART was never needed.

## Development PC

Windows, with the OpenSSH client (`ssh.exe`); the ARM build runs in WSL. No other tools are
needed on the PC.
