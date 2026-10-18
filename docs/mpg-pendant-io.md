# MPG pendant I/O — wheel, axis-select, multiplier, dead-man, E-stop — RESOLVED

_2026-10-16. Live tests on the desk unit, `ddcsv4.out` stopped (so nothing else drives
the buses), pendant MBL-100P-5L on the dedicated 15-pin MPG port. Raw logs:
[raw/2026-10-16_mpg-pendant-io/](raw/2026-10-16_mpg-pendant-io/) (`w4_*` = the run
that decoded everything)._

## Answer

The pendant's switches are **plain parallel contact lines** on the 15-pin MPG port
(`XIN YIN ZIN AIN X1 X10 X100 ESTOP`, each closing to `COM-`). They go through the
board's **opto-inputs** and the **FPGA**, which exposes them as one ordinary FPGA bus
register, **`0x3D`**. Read it like any other register (`MOTION_IOC_READ`, addr `0x3D`).
No serial protocol, no TM1638, no SoC GPIO. The wheel is decoded separately by the FPGA
into **`0x0E`**.

| Pendant control | Electrical (MPG port) | Register / bit | Polarity | Gated by dead-man? |
|---|---|---|---|---|
| Wheel (100 ppr) | Differential quadrature `WHA±` `WHB±` | `0x0E`, 16-bit up/down count, x4 | count | **No** |
| Multiplier **X1** | `X1` → `COM-` | `0x3D` bit 0 | active-low | yes |
| Multiplier **X10** | `X10` → `COM-` | `0x3D` bit 1 | active-low | yes |
| Multiplier **X100** | `X100` → `COM-` | `0x3D` bit 2 | active-low | yes |
| Axis **X** | `XIN` → `COM-` | `0x3D` bit 3 | active-low | yes |
| Axis **Y** | `YIN` → `COM-` | `0x3D` bit 4 | active-low | yes |
| Axis **Z** | `ZIN` → `COM-` | `0x3D` bit 5 | active-low | yes |
| Axis **4** | `AIN` → `COM-` | `0x3D` bit 6 | active-low | yes |
| Axis **5**, **6**, **OFF** | no pin (the pendant cable has no wires for 5/6) | none of bits 3-6 low | | |
| RESET / E-stop (NC) | `ESTOP` → `COM-` | `0x3D` bit 9 | **active-high** (1 = pressed/open) | **No** |
| Dead-man (side switch) | no pin of its own | not directly visible | | (it is the gate) |

Bits 7 and 8 read `1` and never changed, so they're probably unused or meant for more
axes. Idle value with nothing active: **`0x01FF`**. Defines are in
[../driver/motiondev_hw.h](../driver/motiondev_hw.h) (`FPGA_REG_MPG_INPUTS`, `MPG_IN_*`).

**How the dead-man works:** it switches the common of the axis and multiplier contacts
inside the pendant. With it released, bits 0-6 all read `1` whatever the dials are set
to (confirmed: changing axis to Y and multiplier to X10 without the dead-man gave no
change). The moment it's held, the current axis bit and multiplier bit drop together.
So software can treat "any of bits 0-6 low" as "dead-man held". The wheel count and the
E-stop bit are **not** gated, so the app has to ignore `0x0E` deltas itself while no
axis bit is low.

**E-stop side effects:** pressing it also set `0x1D` bit 1 (`0x0000 → 0x0002`) and
pulsed `0x00` to `0x0800` for one sweep, on both press and release. These look like a
latched E-stop/alarm status and an event/interrupt flag. Not decoded further.

## Wiring requirement (the reason nothing worked at first)

The opto-inputs need the **I/O supply on the 37-pin connector: `COM+` (pin 20) = +24 V,
`COM-` (pin 1) = 0 V**. Pins 19 (`24V`) + 37 (`GND`) only power the logic. With only
19 + 37 wired:

- `0x3D` sat at `0x03FF`: E-stop permanently "pressed", every other input inactive.
- The app showed RESET/E-stop always active and ignored the pendant.
- Only the wheel worked, because the quadrature receiver runs from the logic side.

After the user wired `COM+`/`COM-` to the 24 V supply, `0x3D` dropped to `0x01FF` and
every switch decoded cleanly.

## What was measured

| Run | Wiring | Recorders | Result |
|---|---|---|---|
| 1 | 19 + 37 only | `fpgabus watch_all` | Nothing (wheel turned too little) |
| 2 | 19 + 37 only | `fpgabus watch_all` | Only `0x0E` (wheel). Axis/multiplier with dead-man held: no change anywhere |
| 3 | 19 + 37 only | FPGA + TM1638 + GPIO | Only `0x0E`. TM1638 stayed `00 00 00 00`, no non-bus GPIO moved |
| **4** | **+ COM+/COM-** | FPGA + TM1638 + GPIO | **Full map above**, all from `0x3D`. TM1638 and GPIO still silent |

Run 4 timeline (`0x3D`; the actual sequence: dead-man hold/release/hold, axis
X→Y→Z→4→5→6 and back, multiplier X10→X100, dead-man release/hold, wheel, release,
Y + X10 without the dead-man, E-stop):

```
45.7  1FE  dead-man held (X1)        93.4-93.8  back 4,Z,Y,X,OFF
51.7  1FF  released                  105.1 1F5  X10 (axis X)
57.6  1FE  held                      110.0 1F3  X100
60.5  1F6  axis X                    118.7 1FF  dead-man released
66.3  1EE  axis Y                    127.6 1F3  held again
71.7  1DE  axis Z                    130-134    wheel, 0x0E 130 counts
77.1  1BE  axis 4                    140.7 1FF  released; Y+X10 -> no change
81.8  1FE  axis 5 / 6 (no line)      163.8 3FF  E-stop pressed / 172.8 1FF released
```

## Method notes

- **`gpioscan watch <s>`** (added to [../mockup/ioctl-probe/gpioscan.c](../mockup/ioctl-probe/gpioscan.c)):
  a bit is reported only after it has held its new value for 500 consecutive samples,
  so floating pins stay silent. It's silent on an idle board. While `fpgabus` or
  `tm1638read` run alongside it, their own bus pins (gpio 17, 20, 49-55, 64-74, 80-84,
  118, 122-124) flood the log and have to be filtered out.
- Running two FPGA-bus tools at once corrupts single reads (seen as blips that read back
  another register's value). Only one `fpgabus` at a time.
- `nohup ... &` over dropbear SSH survives the session closing. The tools `fflush()`
  every sweep, so a 0-byte log means "no change", not lost output.
- The FPGA address bus is 8 bits, so `watch_all 0 0xff` covers the whole register space.
