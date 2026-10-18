# Connector pinouts — from the PCB silkscreen (photos, 2026-10-16)

_Read directly off the enclosure/board printing - no probing or guessing needed for these
three. This answers the "do we need to map the driver connector" question from earlier:
**yes, and here it is**, straight from the silkscreen._

## Main 37-pin connector — axis drivers + I/O

2-row connector, pins 1-19 (bottom row) and 20-37 (top row):

| Pin | Signal | | Pin | Signal |
|---|---|---|---|---|
| 1 | COM- | | 20 | COM+ |
| 2 | VSO | | 21 | OUT1 |
| 3 | OUT2 | | 22 | OUT3 |
| 4 | IN13 | | 23 | IN01 |
| 5 | IN02 | | 24 | IN03 |
| 6 | IN04 | | 25 | IN05 |
| 7 | IN06 | | 26 | IN07 |
| 8 | IN08 | | 27 | IN09 |
| 9 | IN10 | | 28 | IN11 |
| 10 | IN12 | | 29 | **XP+** |
| 11 | **XP-** | | 30 | **XD+** |
| 12 | **XD-** | | 31 | **YP+** |
| 13 | **YP-** | | 32 | **YD+** |
| 14 | **YD-** | | 33 | **ZP+** |
| 15 | **ZP-** | | 34 | **ZD+** |
| 16 | **ZD-** | | 35 | **AP+** |
| 17 | **AP-** | | 36 | **AD+** |
| 18 | **AD-** | | 37 | GND |
| 19 | 24V | | | |

**Axis outputs are differential pulse/direction pairs** - `{X,Y,Z,A}P±` (step/pulse) and
`{X,Y,Z,A}D±` (direction), one +/- pair each, matching the FPGA's own 4-axis design
(X/Y/Z/A, consistent with the screen showing an A axis alongside X/Y/Z). `IN01`-`IN13`
(13 general inputs - limit switches, probe, etc.), `OUT1`-`OUT3` (3 outputs, e.g. spindle
enable/direction/coolant), `COM+`/`COM-` (input/output commons), `VSO` (**spindle analog
0–10 V output**, confirmed 2026-10-18; see [io-map.md](io-map.md)), `24V`/`GND` (power).

This is the connector [hardware-setup.md](hardware-setup.md) describes as "mostly
unplugged" on the desk setup (only `COM` and `24V` wired for display power) - so none of
`IN01-13`, `OUT1-3`, or any axis P/D line is currently connected to anything.

## HMI/RS232 port (DB9) — not connected; possibly for an external display

```
   9   5
  GND GND
 TXD2 TXD1
 RXD2 RXD1
  5V   5V
   6   1
```
Pin 1/6=5V, 2/7=RXD1/RXD2, 3/8=TXD1/TXD2, 4/9=GND/GND (two complete, apparently
independent TXD/RXD/5V/GND groups on one DB9). **Nothing is plugged in here** - initially
mistaken for the MPG's connector (the user first described its wire labels, before
spotting from the photos that the MPG cable is actually seated in the dedicated 15-pin
MPG port below). Named "HMI/RS232" on the silkscreen, so it's plausibly meant for an
optional external display/HMI panel sold for this controller - but the protocol it
expects (if any beyond a generic serial link) hasn't been investigated. Worth a firmware
string search or a vendor manual check before assuming any generic "screen" would work
with it.

## Dedicated 15-pin "MPG 手轮" port — **this is where the handwheel is plugged in**

```
  15        8
 ESTOP    COM-
  X10      X1
  AIN     X100
  YIN     ZIN
 WHA-     XIN
 WHB-    WHA+
  GND    WHB+
           +5V
   9        1
```

Confirmed (from the photos) as the connector the MPG pendant actually uses. This carries
**true differential quadrature** - `WHA+/WHA-` and `WHB+/WHB-`, real A/B pulse pairs -
exactly matching the user's original description of the pendant ("4 digital signals...
pulse stream... not an absolute dial", see [mpg-investigation.md](mpg-investigation.md)).

**Correction:** the pendant is NOT encoder-only, as first assumed from the product
listing name - a photo of the actual unit shows it's a full-featured pendant with:
- a red twist-release **RESET** button (physically styled as an E-stop; almost certainly
  wired to the `ESTOP` pin)
- an **axis-select** rotary switch: `OFF, X, Y, Z, 4, 5, 6`
- a **multiplier** rotary switch: `X1, X10, X100`
- a small ±/direction LED indicator next to the axis switch

So this pendant most likely uses the connector's `XIN/YIN/ZIN/AIN` (axis-select, one
pulled to `COM-` per switch position - note the switch goes up to `6`, beyond the board's
4 labeled axis lines; positions `5`/`6` may be unused/reserved), `X1/X10/X100`
(multiplier), and `ESTOP`, in addition to `WHA±`/`WHB±` - not yet individually confirmed
live the way the encoder count, buttons, and LEDs were. **Mapped 2026-10-16:** all of
them land in FPGA register `0x3D` (active-low opto inputs; they need `COM+`/`COM-` on the
37-pin connector fed with 24 V) - bit map in [mpg-pendant-io.md](mpg-pendant-io.md).

## What this resolves

- The handwheel's signal genuinely is raw differential quadrature, as originally
  described - a differential receiver on the board converts it to single-ended logic
  that feeds the **FPGA directly** (not the SoC's TIMROT peripheral, which was cleanly
  ruled out earlier), consistent with the live count only being readable via
  `FPGA_REG_MPG_COUNT` (`0x0E`) and never showing any activity on the SoC's own GPIO
  pins. Exactly the "decoded in the FPGA for exactness" hunch from the start of this
  investigation.
- The pendant's axis-select and multiplier switches are a **separate, not-yet-confirmed**
  input path from the front-panel buttons mapped in [ui-controls-map.md](ui-controls-map.md)
  (none of those 18 correspond to axis-select/multiplier) - consistent with them coming
  from the pendant's own switches over the MPG connector instead.
- Gives the **full axis-driver I/O map** for whenever real drivers get connected - no
  further hardware tracing needed for the 37-pin connector.
- Leaves the **HMI/RS232 port's exact protocol** as the one remaining open question from
  this round of silkscreen reading, if an external display is ever wanted.
