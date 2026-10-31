# Hardware setup

## On the machine (garage)

_Since 2026-10-31._ The unit is mounted on the CNC machine in the garage, with the drives and
the limit/home switches connected.

- **Limit/home switches:** NC, one per axis, on **IN02 / IN03 / IN04** (X / Y / Z); closed at
  rest (`0x37` = `0xFFF1`), `limits.level_high` = 1. They are **not** handed to the FPGA's own
  limit function (it treats closed as active and froze every axis): limits and homing are in
  software only, see [fpga-motion.md](fpga-motion.md) 2026-10-31.
- **E-stop:** IN01, `estop.level_high` = 0. **Drive alarms:** IN10–IN13.
- **Drives:** stepper drives at **2000 pulses/rev** (DIP switches) on 5 mm pitch spindles,
  so **400 steps/mm** on X, Y and Z. NEMA23 motors (200 full steps/rev).
- **X DIR output of the controller is defective:** it swings −3 V / +0.77 V instead of ±3 V,
  also with nothing connected (measured by the owner), so the X drive saw one direction only.
  The **X drive is wired to the A connector** (`AP±`/`AD±`): `axis.X.output` = `A`,
  `axis.A.output` = `-`. X keeps its own `dir_invert` (it belongs to the drive's wiring).
- **Switch overtravel vs. braking:** limit stops and the E-stop brake on the ramp
  (`motion.graceful_stop`), and braking is not blocked by a tripped switch. Travel past the
  switch point = v·t_tick (the switch is polled once per tick, up to 34 ms seen) + v·t_burst
  (one burst in flight, ~30 ms) + v²/2A + vA/2J, at the highest speed the axis reaches
  (`max_vel`, also G0; settings of 2026-10-31):

  | axis | max_vel | accel / jerk | detection | in flight | braking | **total** |
  |---|---|---|---|---|---|---|
  | X | 1200 mm/min (20 mm/s) | 300 / 10000 | 0.68 mm | 0.60 mm | 0.67 + 0.30 mm | **≈2.3 mm** |
  | Y | 1200 mm/min (20 mm/s) | 300 / 10000 | 0.68 mm | 0.60 mm | 0.67 + 0.30 mm | **≈2.3 mm** |
  | Z | 1200 mm/min (20 mm/s) | 300 / 10000 | 0.68 mm | 0.60 mm | 0.67 + 0.30 mm | **≈2.3 mm** |

  Every switch needs more overtravel than that (check by hand before relying on it).
  Raising `max_vel` (possible at 400 steps/mm) grows it fast: 40 mm/s gives ≈6.5 mm.
- **Settings history on the unit:** `/ddcsv4/seaotter.cfg.2026-10-31` was saved *after* the
  remap and the 400 steps/mm change. Before that day: X/Y/Z 2000 steps/mm, every axis on its
  own connector (output = its own letter), no enable output. Binaries kept: `seaotter.local-193`
  (before the FPGA limit fix), `-195`, `-201` (incident fix), `-214`.
- **Network:** no wired LAN in the garage. A TP-Link TL-WR1043ND v2 with OpenWrt 25.12 + relayd
  bridges the home wifi to its LAN ports; the unit is on LAN port 4 (100 Mbit), still static
  `192.168.1.200`. Details in [network.md](network.md).
- **Not yet verified on the machine:** each limit switch in both directions, homing, and the
  E-stop during motion. Run no real program before that has passed.

## Desk test setup (until 2026-10-31)

_Since 2026-10-15._

The unit is on the desk next to the dev PC, not in the CNC machine (that stays in the garage).

- **Power:** 24V bench adapter. Two thin solid wires go into **pin 19 (24V)** and **pin 37 (GND)**
  of the large main (37-pin) connector. Since 2026-10-16 also **COM+ (pin 20) = +24V and
  COM- (pin 1) = 0V** from the same supply - required for the opto inputs (pendant
  switches, E-stop); without it the app sits in permanent E-stop. The rest of that connector is unplugged -
  full pinout now known from the silkscreen, see [connector-pinouts.md](connector-pinouts.md).
- **Attached:** only the MPG pendant (MBL-100P-5L), plugged into the **dedicated 15-pin
  "MPG" port** (corrected 2026-10-16 from photos - an earlier note here said the
  HMI/RS232 port, which was a label mix-up; see [connector-pinouts.md](connector-pinouts.md)).
  Wheel = FPGA `0x0E`, switches = FPGA `0x3D` - see [mpg-pendant-io.md](mpg-pendant-io.md).
- **Not attached:** stepper/servo drivers, spindle, limit switches, the 37-pin connector's
  I/O. Nothing can
  move, and none of the 37-pin connector's inputs/outputs will show any activity.
- **Ethernet:** the unit has an RJ45 port, static IP `192.168.1.200/24` (see [network.md](network.md)).
- **Shell access:** solved - persistent root SSH via dropbear, see [ssh-access.md](ssh-access.md).
  UART was never needed in the end.

## Dev PC

- Windows 11, Intel I225-V Ethernet, `192.168.1.65/24` via DHCP, gateway `192.168.1.1`.
- OpenSSH client available (`ssh.exe`). No nmap or PuTTY installed.
