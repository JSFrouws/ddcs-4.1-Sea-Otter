# Desk test setup

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
