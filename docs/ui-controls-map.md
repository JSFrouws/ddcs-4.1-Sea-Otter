# Front panel: buttons, MPG, and what's still open

_2026-10-16. The panel is a **physical membrane keypad around a plain LCD** - not a
touchscreen, despite `/dev/input/event0` ("MXS touchscreen") existing on the system
(apparently vestigial/unused hardware capability on this board). All 18 buttons are
scanned via the **TM1638 chip** (serial STB/DIO/CLK protocol, GPIO1 pins 49/50/51),
confirmed with [../mockup/ioctl-probe/tm1638read.c](../mockup/ioctl-probe/tm1638read.c) -
a direct `mmap()`-based replica of `driver/motiondev_lld.c`'s key-scan routine, run with
the real app stopped (so there's no bus contention - see the method note below)._

## Method

Each button was identified by **repeated, grouped presses** (5x in a row per button),
watched live with `tm1638read watch <seconds>`, which only prints the 4-byte scan when it
changes. A first attempt at inferring the map from a single continuous press-through of
all 18 buttons was **wrong** (one button's press apparently didn't register as a distinct
transition, silently shifting every label after it by one) - the grouped-repeat method
below is what actually nailed it down, confirmed batch by batch.

## Confirmed button map (all 18, no collisions)

Scan code is 4 bytes (`byte0 byte1 byte2 byte3`), one bit set per button. Key code is
`byte_index*8 + bit + 1`, matching `driver/motiondev_lld.c`'s `motiondev_lld_key_poll()`
formula (so `MOTION_IOC_KEY` would report these same numbers, bit 0x80 ORed in for
"pressed" - **once that ioctl path is fixed to actually read TM1638**, see below).

| Button | Scan code | byte.bit | Key code |
|---|---|---|---|
| Start | `80 00 00 00` | 0.7 | 8 |
| Tab | `40 00 00 00` | 0.6 | 7 |
| Select A | `20 00 00 00` | 0.5 | 6 |
| Pause | `08 00 00 00` | 0.3 | 4 |
| Shift | `04 00 00 00` | 0.2 | 3 |
| Modify/Probe | `02 00 00 00` | 0.1 | 2 |
| Reset | `00 80 00 00` | 1.7 | 16 |
| Mode | `00 40 00 00` | 1.6 | 15 |
| Cancel/Z+ | `00 20 00 00` | 1.5 | 14 |
| View | `00 08 00 00` | 1.3 | 12 |
| Y+ | `00 04 00 00` | 1.2 | 11 |
| Enter/Home | `00 02 00 00` | 1.1 | 10 |
| TryCut | `00 00 80 00` | 2.7 | 24 |
| Zero (Y-) | `00 00 40 00` | 2.6 | 23 |
| X+ | `00 00 20 00` | 2.5 | 22 |
| Spindle | `00 00 08 00` | 2.3 | 20 |
| Page R | `00 00 00 80` | 3.7 | 32 |
| Goto0 (X-) | `00 00 00 08` | 3.3 | 28 |

Clean, self-consistent pattern: the same 6 bit positions (1,2,3,5,6,7) repeat across
byte0/byte1, and the same 4 (3,5,6,7) repeat across byte2/byte3 - exactly what a
row/column scan matrix looks like, with bits 0 and 4 apparently unused (maybe
reserved/unwired matrix positions).

## Not yet mapped

- ~~Arrow-pad buttons~~ **closed 2026-10-18**: the six arrow keys are the same physical
  keys already in the table, with no extra keys. Each key carries multiple legends:

  | Key | Printed legend (top → bottom) | Scan code | Key code |
  |---|---|---|---|
  | X+ | X+ / x | `00 00 20 00` | 22 |
  | X− | X− / GOTO 0 | `00 00 00 08` | 28 |
  | Y+ | Y+ / y | `00 04 00 00` | 11 |
  | Y− | Y− / zero | `00 00 40 00` | 23 |
  | Z+ | Z+ / Cancel / z | `00 20 00 00` | 14 |
  | Z− | Z− / ENTER / HOME | `00 02 00 00` | 10 |

  Verified by pressing each key twice and then all six once, with the TM1638 and the
  FPGA recorded (the FPGA showed nothing: the front panel is TM1638-only). Raw log:
  [raw/2026-10-18_arrow-keys.txt](raw/2026-10-18_arrow-keys.txt).
- **The MPG pendant's own buttons/switches** - now covered in
  [mpg-pendant-io.md](mpg-pendant-io.md) (the pendant does have axis/multiplier/E-stop
  switches on the 15-pin MPG port; the text below is the earlier, outdated assumption). The purchased unit
  ("Handwheel handling unit MPG 100ppr", model MBL-100P-5L) was described as having
  exactly 4 signal wires (`TXD1 TXD2 RXD1 RXD2`) + power/ground, which is consistent with
  **encoder-only** (two differential quadrature pairs, no auxiliary buttons on this
  cable) - but this hasn't been explicitly confirmed. If the physical pendant has any
  buttons on its housing, they'd need either more wires than described or a multiplexed
  protocol, so they're probably not present/wired - worth a quick visual check of the
  pendant itself.

## `MOTION_IOC_KEY` is broken the same way `MOTION_IOC_MPG_COUNT` was

A clean 60-second capture through the vendor's own loaded driver (`MOTION_IOC_KEY` via
`/dev/motion`, app stopped) caught **zero** key events despite buttons being pressed
during that window (see [mpg-investigation.md](mpg-investigation.md) for that test). This
is the same pattern as the MPG count: whatever the real V4.1 firmware/driver does for key
reads isn't matching our `motiondev_lld_key_poll()` port's assumptions, even though the
TM1638 **hardware** protocol itself clearly does work (confirmed directly, as above). Not
yet root-caused - possibilities: a different ioctl is actually used for keys, the
`FPGA_REG_KEYS_EXT` byte matters in a way we haven't captured, or there's a polling/edge
nuance the vendor driver handles differently. Flagging as an open item rather than
guessing further tonight.

## LED control - RESOLVED, all 7 confirmed

`motiondev_lld_led_write(digit, segments)` (`tm1638_command(0x44)` fixed-address mode,
write digit+segments, `0x8F` display-on) drives all of them directly. Tested live with
[../mockup/ioctl-probe/tm1638led.c](../mockup/ioctl-probe/tm1638led.c) (same `mmap()`
approach as the key/MPG tools, app stopped to avoid fighting its own LED refresh).

All 7 physical LEDs sit at **odd TM1638 addresses** (1, 3, 5, 7 - the even addresses are
presumably the 7-segment digit positions this chip also supports, unused here; address 9
was tried and is empty):

| LED | Digit (address) | Bit |
|---|---|---|
| Start | 1 | `0x01` |
| Ethernet (status, top right) | 1 | `0x02` |
| Pause | 3 | `0x01` |
| USB Disk (status, top right) | 3 | `0x02` |
| Reset | 5 | `0x01` |
| Shift | 5 | `0x02` |
| Spindle | 7 | `0x01` |

Each address holds exactly the bits shown - the others are unwired (writing them has no
visible effect). No LEDs were found for View, TryCut, Page R, Tab, Select A, Modify/Probe,
Mode, or Cancel/Z+.

**View and TryCut specifically were expected to have LEDs** (the panel's indicator dots
suggest all 7 bottom-row buttons should), but an exhaustive sweep of **every valid TM1638
address (0-15) and every bit** found nothing for either. Addresses >15 (tried: 17/19/21/23
decimal) just alias back to address mod 16 (confirmed: `digit=17` lit the same LEDs as
`digit=1` - the chip's address field is 4 bits), so there is nowhere else on this bus left
to check. Tested with the real app both running and stopped, including deliberately
entering `TryCut`'s mode on the real app - its LED never lit. Instead, the app shows a
**`CONT` indicator turning blue on-screen** when `TryCut` is engaged, suggesting the app's
own idea of "show TryCut is active" is the on-screen indicator, not the physical LED - so
either that LED position isn't physically populated on this board, or the firmware has a
gap and never asserts it. Either way, not something further bit-probing will resolve.
**Closed as: not wired / not driven, confirmed by exhaustive address sweep.**

To drive one: `motiondev_lld_led_write(digit, bit)` with the digit/bit pair from the table
(e.g. `motiondev_lld_led_write(5, 0x01)` lights Reset). Multiple LEDs at the same digit can
be combined by ORing their bits (confirmed: `digit=5, segments=0x03` lights both Reset and
Shift together).
