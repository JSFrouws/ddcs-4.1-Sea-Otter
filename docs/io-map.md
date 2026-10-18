# DDCS V4.1 I/O map — every connector pin and panel control → SoC / FPGA

_Overview, 2026-10-18. One table per connector with where each signal lands and how
it was proven. Details and raw data are in the linked docs. FPGA registers are accessed
over the SoC's GPIO bus (`MOTION_IOC_READ` / `MOTION_IOC_WRITE` via `/dev/motion`, or
`fpgabus` / `motionw` from userspace). **Read and write registers at the same address
are different things.**_

## Connectors on the unit

Four I/O connectors, plus USB and Ethernet (photo:
[raw/2026-10-18_back-panel.jpg](raw/2026-10-18_back-panel.jpg)):

| Connector | Type | Role |
|---|---|---|
| **Main Port** | 37-pin D-sub | axis pulse/dir ×4, IN01–IN13, OUT1–OUT3, I/O and logic supply |
| **MPG 手轮** | 15-pin D-sub | handwheel pendant (quadrature, axis/multiplier selectors, E-stop) |
| **HMI/RS232 串口** | DB9 | two serial groups for an optional external HMI (SoC AUART0/1, unused) |
| **IN14–IN18 + COM** | 6-pin green pluggable terminal | extra inputs; IN15–18 are the external-button inputs |
| **USB** ×2 | USB-A host | one is cabled to the user's CNC operator box. USB stick was the stock way to copy `.nc` programs onto the machine (mounted as `/udisk-sda*`, see [rootfs-findings.md](rootfs-findings.md)) |
| **Ethernet 网络** | RJ45 | `192.168.1.200`: used for SSH now ([ssh-access.md](ssh-access.md)), planned for the future API |

**Spindle speed** has two options in the app (parameter #188): **Analog**, where SoC
PWM6 (ioctl `0x4627`) drives an on-board filter to **VSO (main port pin 2) = 0–10 V**,
linear in duty (confirmed by meter), or **PUL/DIR**, where the spindle borrows one axis's
pulse/dir pair (#189, X/Y/Z/A) and speed = pulse frequency. OUT1–3 are on/off only and
can't be set to PWM. The 24 V that was read on VSO earlier, with the app stopped and the
PWM state undefined, wasn't reproduced. With the PWM explicitly set, VSO stays within 0–10 V.

## Status

| Connector / area | Mapped | Open |
|---|---|---|
| Front panel (keys, LEDs, beeper, LCD) | all 18 keys (incl. the 6 multi-legend arrow keys), 7 LEDs | — |
| 15-pin MPG port | **13/13 signals** | — |
| 37-pin main connector | **37/37 pins** | — |
| 6-pin terminal IN14–IN18 + COM (back) | **6/6** | — |
| DB9 HMI/RS232 | SoC side identified (2 AUARTs, unused) | pin ↔ `ttySP0`/`ttySP1` assignment (loopback test). Not needed unless an HMI is wanted |
| Spindle analog (SoC PWM6) | **VSO, main port pin 2, 0–10 V** (meter) | — |
| FPGA read regs `0x38` bits 2–8 / `0x3C` bits 0–3 | — | idle all-ones, no pin found on any connector (maybe unpopulated inputs) |

## Front panel (SoC side)

| Control | Path | Details |
|---|---|---|
| 18 membrane keys | TM1638 key scan, SoC GPIO 49 (STB) / 50 (DIO) / 51 (CLK) | [ui-controls-map.md](ui-controls-map.md) |
| 7 LEDs | same TM1638, odd digit addresses 1/3/5/7 | [ui-controls-map.md](ui-controls-map.md) |
| Beeper | SoC GPIO 125 (ioctl `0x4623`/`0x4624`) | [motiondev-ko.md](motiondev-ko.md) |
| LCD | `/dev/fb0` | — |
| Touch controller | `/dev/input/event0`, not fitted/used | [ui-controls-map.md](ui-controls-map.md) |

## 15-pin MPG port → FPGA

| Pin signal | FPGA | Notes |
|---|---|---|
| `WHA±`, `WHB±` | read `0x0E`, 16-bit up/down count (x4) | not gated by the dead-man |
| `X1` / `X10` / `X100` | read `0x3D` bit 0 / 1 / 2, active-low | only while dead-man held |
| `XIN` / `YIN` / `ZIN` / `AIN` | read `0x3D` bit 3 / 4 / 5 / 6, active-low | only while dead-man held; pendant has no wires for axes 5/6 |
| `ESTOP` | read `0x3D` bit 9, **1 = pressed** | also sets read `0x1D` bit 1 |
| `+5V`, `GND`, `COM-` | power / switch return | inputs need `COM+`/`COM-` fed on the 37-pin |

Details: [mpg-pendant-io.md](mpg-pendant-io.md).

## 37-pin main connector

| Pin | Signal | Maps to | Proven by |
|---|---|---|---|
| 23, 5, 24, 6, 25, 7, 26, 8, 27, 9, 28, 10, 4 | **IN01 … IN13** | read **`0x37` bit 0 … 12**, active-low (pulled to `COM-`); IN14–18 continue on the terminal block below | wire to COM- per pin, 2026-10-18 |
| 21 / 3 / 22 | **OUT1 / OUT2 / OUT3** | write **`0x3E` bit 0 / 1 / 2** | own write + meter |
| 29/11, 31/13, 33/15, 35/17 | **XP±, YP±, ZP±, AP±** | pulse generator, axis select write `0x00` = 0/1/2/3; idle polarity = per-axis write `0x02` bit 2 | own write + meter (polarity); pulses known good on the user's machine |
| 30/12, 32/14, 34/16, 36/18 | **XD±, YD±, ZD±, AD±** | move command write `0x04` bit 1; invert = per-axis write `0x02` bit 1 | own write + meter |
| 20 / 1 | COM+ / COM- | opto I/O supply (24 V / 0 V), **required** for all inputs | — |
| 2 | **VSO** | **spindle analog 0–10 V output**, from SoC PWM6 (ioctl `0x4627`, duty 0…3000): measured 0.03 / 5.0 / 10 V at duty 0 / 1500 / 3000 vs COM-, i.e. V ≈ duty/300 | own ioctl + meter, 2026-10-18 |
| 19 / 37 | 24V / GND | logic supply | — |

Details: [fpga-outputs.md](fpga-outputs.md) (outputs, axis pins, move command,
`0x02`).

## 6-pin terminal block "IN14 IN15 IN16 IN17 IN18 COM" (back, next to a red LED)

Photos: [raw/2026-10-18_back-panel.jpg](raw/2026-10-18_back-panel.jpg),
[raw/2026-10-18_in14-18-terminal.jpg](raw/2026-10-18_in14-18-terminal.jpg).

| Terminal | Maps to (read, active-low) | Also (read `0x3B`, active-high) |
|---|---|---|
| IN14 | `0x37` bit 13 | — |
| IN15 | `0x37` bit 14 | bit 0 |
| IN16 | `0x37` bit 15 | bit 1 |
| IN17 | `0x38` bit 0 | bit 2 |
| IN18 | `0x38` bit 1 | bit 3 |
| COM | **same as `COM-`** of the main port (IN14 to main COM- worked the same; IN14 to 24V did nothing) | |

So the 18 inputs form one continuous field: **IN*n* = bit *n*−1 of the 32-bit word
`0x38:0x37`**. IN15–IN18 are mirrored into **`0x3B`**, the register the vendor driver
reads on every key poll (`FPGA_REG_KEYS_EXT`). That makes them the controller's
external-button inputs (e.g. external start/pause), fed into the same key-event path as
the front panel. Raw log: [raw/2026-10-18_inputs/in14-18_fpga.txt](raw/2026-10-18_inputs/in14-18_fpga.txt).

### Input side effects (FPGA-internal input functions)

Some inputs also set per-axis status bits in the FPGA, i.e. the FPGA's own limit / home /
alarm logic, assigned by the app's input settings. Observed with the app's current
settings:

| Input | Also sets (read) |
|---|---|
| IN01 | `0x1D` bit 0 (E-stop sets bit 1 of the same register: an alarm/stop latch) |
| IN02 / IN03 / IN04 | `0x1E` / `0x1F` / `0x20` = `0x0030` (X / Y / Z) |
| IN10 / IN11 / IN12 / IN13 | `0x1E` / `0x1F` / `0x20` / `0x21` = `0x0100` (X / Y / Z / A) |
| IN05–IN09 | nothing (plain inputs) |

The *meaning* of `0x30` vs `0x100` (limit vs home?) follows from the app's input-function
settings and hasn't been cross-checked. Raw log:
[raw/2026-10-18_inputs/in_fpga.txt](raw/2026-10-18_inputs/in_fpga.txt).

## DB9 "HMI/RS232"

Two independent TXD/RXD/5V/GND groups ([connector-pinouts.md](connector-pinouts.md)).
The SoC has exactly two application UARTs, `ttySP0` (AUART0, `0x8006A000`) and
`ttySP1` (AUART1, `0x8006C000`), and their counters show 0 bytes sent/received: the
stock app never opens them, and no getty runs on them. The Linux console is the
separate debug UART `ttyAM0` (115200, internal header). So: most likely a port for an
optional external HMI panel, wired to AUART0/1, unused by this firmware. Which group is
which UART is unconfirmed.
