# Machine settings — the real `setting` parameter file, decoded

_2026-10-17. Source: `setting` and `setting-bk` from
[raw/sysdisk-2026-10-15/](raw/sysdisk-2026-10-15/) (the SYSDISK backup taken
2026-10-15, SHA256SUMS included). Decoder:
[../tools/decode_settings.py](../tools/decode_settings.py) (stdlib, read-only).
This is the **real garage CNC's saved configuration** — not the desk bench
setup described in [hardware-setup.md](hardware-setup.md), which only has
the MPG and 24V power wired for driver bring-up. `setting` is what the
machine actually runs; `setting-bk` is the factory default the app ships
with, kept alongside it as a backup/reference copy._

## How it was decoded

Both files are a flat array of 1500 little-endian `float64`s (12000 bytes
÷ 8), no header beyond an unused slot 0. `docs/raw/sysdisk-2026-10-15/custom`
turned out to be the app's own parameter catalog — lines like:

```
#018 -t2 -s1"X axis motion direction(pulse/direction mode)" -m1 -min=0.000 -max=1.000 -i0"Negative" -i1"Positive"
```

giving a label, unit, min/max, and enum choices for parameter `#018`. The
parameter number **is** the 0-based index into the `setting` array — i.e.
`#018`'s value is the 8 bytes at offset `18*8 = 0x90`. This was confirmed,
not assumed, two independent ways before trusting it for the whole file:

1. **Cross-validated against hardware already probed directly.** `#018`
   decodes to `1` ("Positive") while Y/Z/A's equivalent (`#019-021`) decode
   to `0` ("Negative") — exactly the **X-axis DIR polarity inversion**
   [fpga-outputs.md](fpga-outputs.md) found independently by measuring
   `XD+`/`XD-` with a multimeter. Likewise `#023-026` ("pulse signal level")
   all read `1` ("high"), matching the measured idle `+3.5V` pulse polarity
   in the same investigation. Two unrelated methods — probing the live FPGA
   bus, and parsing this settings file — landing on the same answer confirms
   both independently.
2. **Cross-validated against already-known network config.** `#327-330` are
   stored as 4 raw address bytes in the first half of the 8-byte slot (not a
   float — read directly, not via `struct.unpack('<d', …)`), and decode to
   `192.168.1.200` / `255.255.255.0` / `192.168.1.1` / `192.168.1.65` — an
   exact match for [network.md](network.md)'s independently-confirmed IP,
   gateway, and shared-host address.

With the index mapping trusted, `decode_settings.py` resolves every
parameter's current value against `custom`'s label/unit/enum-choice table,
and diffs it against `setting-bk` to flag what the owner actually tuned
versus what's still factory default.

## What this tells us about the machine

- **Work envelope:** X 10–915&nbsp;mm, Y 10–590&nbsp;mm, Z −300…−10&nbsp;mm
  *(the vendor config as found. Sea Otter defaults to the machine's real 400 × 200 × 100 mm since 2026-10-23: X 0–400, Y 0–200, Z −100…0)*
  (software limits `#235-242`) — a ~900×580&nbsp;mm router bed with ~290&nbsp;mm
  of Z travel. This replaces the placeholder travel numbers used earlier in
  [architecture.md](architecture.md) and the UI mockup.
- **Steps/mm:** all four axes use pulse-equivalency `2000/1` (`#002-010`) —
  2000 steps per mm on X, Y, Z, and A (A in deg). Factory default was `1000/1`;
  the owner doubled it, consistent with finer microstepping or a different
  pulley/screw than the factory assumption.
- **X axis is electrically inverted relative to Y/Z/A** — both DIR polarity
  (`#018`) and pulse-signal level (`#023-026`), confirmed twice over (see
  above). Any from-scratch motion code must apply this per-axis, not assume
  uniform polarity.
- **Limit and home switches are shared, one per axis, not one per direction.**
  `#141` (X+ limit port) and `#146` (X− limit port) are both port `2`; same
  pattern for Y (`3`) and Z (`4`). `#151-153` (home ports) reuse the **same**
  port numbers again — X home = port 2, Y home = port 3, Z home = port 4.
  So this machine has exactly **one switch per axis**, doing triple duty as
  +limit, −limit, and home reference, with software telling them apart only
  by which way the axis was already moving. This is exactly the "shared
  limit input, ambiguous direction" case [operator-ux.md](operator-ux.md)'s
  limit policy (REQ-A6) was written to handle — it isn't a hypothetical
  edge case, it's how this machine is actually wired.
- **Probe is not wired** (`#156 Probe port = 0`, where `0` reads as "none" —
  every other port field uses `0` only for "unassigned"). External E-stop
  **is** wired, to port `1`. This matches [hardware-setup.md](hardware-setup.md)
  noting the 37-pin I/O connector is "mostly unplugged" — but that note was
  about the *desk* setup; this settings file being the *real machine's*
  saved config with probe=unassigned suggests the garage machine has never
  had a probe wired either, independent of the desk bench state.
- **Home order: Z, X, Y, A** (`#225 = "ZXYA"`) — Z retracts clear before any
  XY motion, exactly the order already assumed in operator-ux.md's Home
  screen design.
- **Home offsets are large and specific** (`#214 X = 925.2`, `#215 Y = 600`)
  — these shift the machine-coordinate home position well away from X0 Y0,
  consistent with home switches sitting at the far corner of a ~915×590
  travel envelope rather than at the work origin.
- **Spindle:** analog 0–10V interface, default/max 12 000/24 000 RPM
  (`#188,191,192`) — already what the mockup assumed, good independent
  agreement. 2 s start-up and off delay (`#194-195`), auto-shutoff at end of
  job enabled (`#412`).
- **MPG wheel:** motion direction **Positive**, control mode **Open** (not
  gated — though [mpg-pendant-io.md](mpg-pendant-io.md)'s live hardware trace
  found the axis/multiplier switches *are* dead-man gated at the electrical
  level regardless of this software setting), precision 0.01.
- **Backlash compensation is disabled on every axis** (`#254-257 = Disable`)
  even though X and Y have a nonzero distance stored (`#259-260 = 0.10mm`) —
  the number is retained but not applied.
- **No tool table populated**: every `H`/`D` length, wear and diameter
  (`#264-311`) is `0`. Tool data in the UI mockup (6 mm flat endmill, etc.)
  is illustrative demo data, not sourced from this machine.
- **Network matches** [network.md](network.md) and [ssh-access.md](ssh-access.md)
  exactly (see above) — further confirming this `setting` file is live,
  current configuration, not a stale export.

## Full parameter reference

Generated by `python -I tools/decode_settings.py`, one table per settings
screen (grouping and screen names per `custom`'s `-mN` tags, matching
`msg-eng`'s `#80-95` screen titles — see [ui-controls-map.md](ui-controls-map.md)'s
sibling research for how `msg-eng` was read). **Value** is this machine's
current setting; **Factory** is `setting-bk`; **changed** flags where they
differ.

### Motor parameters

| # | Parameter | Value | Factory | Unit/choice |
|---|---|---|---|---|
| 1 | Unit selection | 0 | 0 | mm |
| 2 | X axis pulse equivalency(numerator) | 2000 | 1000 | **changed** |
| 3 | Y axis pulse equivalency(numerator) | 2000 | 1000 | **changed** |
| 4 | Z axis pulse equivalency(numerator) | 2000 | 1000 | **changed** |
| 5 | A axis pulse equivalency(numerator) *(4th axis)* | 2000 | 1000 | **changed** |
| 7 | X axis pulse equivalency(denominator) | 1 | 1 | |
| 8 | Y axis pulse equivalency(denominator) | 1 | 1 | |
| 9 | Z axis pulse equivalency(denominator) | 1 | 1 | |
| 10 | A axis pulse equivalency(denominator) *(4th axis)* | 1 | 1 | |
| 12 | X axis drive mode | 0 | 0 | pulse/direction |
| 13 | Y axis drive mode | 0 | 0 | pulse/direction |
| 14 | Z axis drive mode | 0 | 0 | pulse/direction |
| 15 | A axis drive mode *(4th axis)* | 0 | 0 | pulse/direction |
| 17 | Direction-pulse time interval(pulse/direction mode) | 7000 | 7000 | ns |
| 18 | X axis motion direction(pulse/direction mode) | 1 | 0 | Positive **changed** |
| 19 | Y axis motion direction(pulse/direction mode) | 0 | 0 | Negative |
| 20 | Z axis motion direction(pulse/direction mode) | 0 | 0 | Negative |
| 21 | A axis motion direction(pulse/direction mode) *(4th axis)* | 0 | 0 | Negative |
| 23 | X axis pulse signal level(pulse/direction mode) | 1 | 0 | high **changed** |
| 24 | Y axis pulse signal level(pulse/direction mode) | 1 | 0 | high **changed** |
| 25 | Z axis pulse signal level(pulse/direction mode) | 1 | 0 | high **changed** |
| 26 | A axis pulse signal level(pulse/direction mode) *(4th axis)* | 1 | 0 | high **changed** |
| 28 | X axis motion direction(two-pulse mode) | 0 | 0 | Negative |
| 29 | Y axis motion direction(two-pulse mode) | 0 | 0 | Negative |
| 30 | Z axis motion direction(two-pulse mode) | 0 | 0 | Negative |
| 31 | A axis motion direction(two-pulse mode) *(4th axis)* | 0 | 0 | Negative |
| 33 | Enable axis mapping function | 0 | 0 | No |
| 34 | Master axis selection | 0 | 0 | X axis |
| 35 | Slave axis selection | 1 | 1 | Y axis |
| 36 | Enable A axis cyclic encoding *(4th axis)* | 0 | 0 | No |
| 37 | A axis cyclic encoder upper *(4th axis)* | 180 | 180 | unit |
| 38 | A axis cyclic encoder lower *(4th axis)* | -180 | -180 | unit |
| 39 | AB axis Selection *(4th axis)* | 0 | 0 | A axis |

### Manual control parameters

| # | Parameter | Value | Factory | Unit/choice |
|---|---|---|---|---|
| 40 | Motor start speed | 50 | 50 | unit/min |
| 41 | X axis manual control speed | 1200 | 5000 | unit/min **changed** |
| 42 | Y axis manual control speed | 1560 | 5000 | unit/min **changed** |
| 43 | Z axis manual control speed | 1560 | 5000 | unit/min **changed** |
| 44 | A axis manual control speed *(4th axis)* | 5000 | 5000 | deg/min |
| 46 | X axis start Acc in M_Ctrl mode | 160 | 300 | unit/s2 **changed** |
| 47 | Y axis start Acc in M_Ctrl mode | 160 | 300 | unit/s2 **changed** |
| 48 | Z axis start Acc in M_Ctrl mode | 160 | 300 | unit/s2 **changed** |
| 49 | A axis start Acc in M_Ctrl mode *(4th axis)* | 300 | 300 | deg/s2 |
| 51 | X axis stop Acc in M_Ctrl mode | 160 | 600 | unit/s2 **changed** |
| 52 | Y axis stop Acc in M_Ctrl mode | 160 | 600 | unit/s2 **changed** |
| 53 | Z axis stop Acc in M_Ctrl mode | 600 | 600 | unit/s2 |
| 54 | A axis stop Acc in M_Ctrl mode *(4th axis)* | 600 | 600 | deg/s2 |
| 56 | X axis emergency stopAcc in M_Ctrl mode | 160 | 1600 | unit/s2 **changed** |
| 57 | Y axis emergency stopAcc in M_Ctrl mode | 160 | 1600 | unit/s2 **changed** |
| 58 | Z axis emergency stopAcc in M_Ctrl mode | 160 | 1600 | unit/s2 **changed** |
| 59 | A axis emergency stopAcc in M_Ctrl mode *(4th axis)* | 1600 | 1600 | deg/s2 |
| 91 | X Axis max speed in M_Ctrl mode | 1200 | 12000 | unit/min **changed** |
| 92 | Y Axis max speed in M_Ctrl mode | 1200 | 12000 | unit/min **changed** |
| 93 | Z Axis max speed in M_Ctrl mode | 1200 | 12000 | unit/min **changed** |
| 94 | A Axis max speed in M_Ctrl mode *(4th axis)* | 1200 | 12000 | deg/min **changed** |
| 96 | Jog support in CONT mode | 1 | 0 | Yes **changed** |
| 97 | Jog moving distance in CONT mode | 0.01 | 0.01 | unit |
| 98 | Jog-1 moving distance | 0.05 | 0.1 | unit **changed** |
| 99 | Jog-2 moving distance | 1 | 1 | unit |
| 100 | Jog-3 moving distance | 10 | 10 | unit |

### Automatic control parameters

| # | Parameter | Value | Factory | Unit/choice |
|---|---|---|---|---|
| 101 | Speed Selection | 0 | 0 | G code |
| 102 | default operation speed | 1200 | 3000 | unit/min **changed** |
| 103 | G0 Speed | 1200 | 3000 | unit/min **changed** |
| 104 | operation acceleration | 160 | 500 | unit/s2 **changed** |
| 105 | Speed drop rate during pause | 5 | 5 | |
| 106 | Speed drop rate during failure | 20 | 20 | |
| 107 | Uniaxial acc is limited by M_Ctrl start acc | 0 | 0 | No |
| 108 | Restore the machining start point position(when #36=No) | 0 | 0 | Pause line start |
| 109 | Machining accuracy | 0.005 | 0.002 | unit **changed** |
| 110 | arc algorithm chord error | 0.005 | 0.001 | mm **changed** |
| 111 | Circular centrifugal acceleration | 0.5 | 0 | unit/s2 **changed** |
| 112 | Circular speed adjustment factor | 1 | 1 | |
| 113 | maximum speed | 1800 | 8000 | unit/min **changed** |
| 114 | X axis protection speed | 1200 | 99999 | unit/min **changed** |
| 115 | Y axis protection speed | 1560 | 99999 | unit/min **changed** |
| 116 | Z axis lifting protection speed | 1800 | 3000 | unit/min **changed** |
| 117 | A axis protection speed *(4th axis)* | 99999 | 99999 | deg/min |
| 119 | Z axis dropping protection speed | 1000 | 3000 | unit/min **changed** |
| 120 | G0 instruction movement mode | 1 | 1 | Interpolation |
| 121 | Is FRO valid for G0?(when #120=Interpolation) | 1 | 1 | Yes |
| 122 | Macro programming mode | 0 | 0 | Disable |
| 123 | Macro program file main program No. | 0 | 0 | |
| 124 | interpolation period | 0.005 | 0.005 | s |
| 125 | Bias coordinate adjustment step | 1 | 1 | unit |
| 126 | Bias coordinate adjustment speed | 500 | 500 | unit/min |
| 900 | Z axis return to machine safe height when starting machining | 0 | 0 | No |
| 901 | Z axis return to machine safe height when restore machining | 0 | 0 | No |
| 902 | Z axis machine safe height | 5 | 5 | unit |
| 903 | Pause action selection | 0 | 0 | Z-axis lift |
| 904 | Z axis lift distance when pause | 5 | 5 | unit |
| 905 | Tool change command M6 back to reference point 1 pause | 0 | 0 | No |
| 906 | Z axis positioning when goto zero | 0 | 0 | Workpiece safe height |
| 907 | Z axis Workpiece safe height | 15 | 0 | unit **changed** |

### Output signal parameters

| # | Parameter | Value | Factory | Unit/choice |
|---|---|---|---|---|
| 127 | M3 prot | 1 | 1 | |
| 128 | M4 prot | 0 | 0 | |
| 129 | M8 prot | 2 | 2 | |
| 130 | M10 prot | 3 | 3 | |
| 131 | M3 active electric level | 1 | 1 | High |
| 132 | M4 active electric level | 0 | 1 | Low **changed** |
| 133 | M8 active electric level | 1 | 1 | High |
| 134 | M10 active electric level | 1 | 1 | High |

`M3 prot`/`M8 prot`/etc. are the FPGA output-bit assignment (port number),
matching `OUT1-3` in [fpga-outputs.md](fpga-outputs.md): M3→port 1 (OUT1,
spindle), M8→port 2 (OUT2, coolant). M4 and M10 (ports 0 and 3) exist in
config but [fpga-outputs.md](fpga-outputs.md) found no M-code mapped to
port 0 in a live trace — consistent with M4 (spindle reverse) not being
used on this machine.

### Input signal parameters

| # | Parameter | Value | Factory | Unit/choice |
|---|---|---|---|---|
| 135 | IO input filter time width | 20 | 20 | ms |
| 136 | X axis drive alarm prot | 10 | 1 | **changed** |
| 137 | Y axis drive alarm prot | 11 | 2 | **changed** |
| 138 | Z axis drive alarm prot | 12 | 3 | **changed** |
| 139 | A axis drive alarm prot *(4th axis)* | 13 | 0 | **changed** |
| 141 | X axis positive limit port | 2 | 4 | **changed** |
| 142 | Y axis positive limit port | 3 | 5 | **changed** |
| 143 | Z axis positive limit port | 4 | 6 | **changed** |
| 144 | A axis positive limit port *(4th axis)* | 0 | 0 | |
| 146 | X axis negative limit port | 2 | 7 | **changed** |
| 147 | Y axis negative limit port | 3 | 8 | **changed** |
| 148 | Z axis negative limit port | 4 | 9 | **changed** |
| 149 | A axis negative limit port *(4th axis)* | 0 | 0 | |
| 151 | X axis home port | 2 | 10 | **changed** |
| 152 | Y axis home port | 3 | 11 | **changed** |
| 153 | Z axis home port | 4 | 12 | **changed** |
| 154 | A axis home port *(4th axis)* | 0 | 1 | **changed** |
| 156 | Probe port | 0 | 13 | **changed** |
| 157 | External emergency stop port | 1 | 14 | **changed** |
| 158 | Extended Function Key 1 Port | 15 | 15 | |
| 159 | Extended Function Key 2 Port | 16 | 16 | |
| 160 | Extended Function Key 3 Port | 17 | 17 | |
| 161 | Extended Function Key 4 Port | 18 | 18 | |
| 162 | X axis drive alarm active electric level | 0 | 0 | Low |
| 163 | Y axis drive alarm active electric level | 0 | 0 | Low |
| 164 | Z axis drive alarm active electric level | 0 | 0 | Low |
| 165 | A axis drive alarm active electric level *(4th axis)* | 0 | 0 | Low |
| 167 | X axis positive limit port active electric level | 1 | 0 | High **changed** |
| 168 | Y axis positive limit port active electric level | 1 | 0 | High **changed** |
| 169 | Z axis positive limit port active electric level | 1 | 0 | High **changed** |
| 170 | A axis positive limit port active electric level *(4th axis)* | 1 | 0 | High **changed** |
| 172 | X axis negative limit port active electric level | 1 | 0 | High **changed** |
| 173 | Y axis negative limit port active electric level | 1 | 0 | High **changed** |
| 174 | Z axis negative limit port active electric level | 1 | 0 | High **changed** |
| 175 | A axis negative limit port active electric level *(4th axis)* | 1 | 0 | High **changed** |
| 177 | X axis home active electric level | 1 | 0 | High **changed** |
| 178 | Y axis home active electric level | 1 | 0 | High **changed** |
| 179 | Z axis home active electric level | 1 | 0 | High **changed** |
| 180 | A axis home active electric level *(4th axis)* | 1 | 0 | High **changed** |
| 182 | Probe active electric level | 0 | 0 | Low |
| 183 | External emergency stop active electric level | 0 | 0 | Low |
| 184 | Extended Function Key 1 active electric level | 0 | 0 | Low |
| 185 | Extended Function Key 2 active electric level | 0 | 0 | Low |
| 186 | Extended Function Key 3 active electric level | 0 | 0 | Low |
| 187 | Extended Function Key 4 active electric level | 0 | 0 | Low |

As noted above: ports `2`/`3`/`4` are each claimed by *three* roles (+limit,
−limit, home) per axis — X, Y, Z all share one physical switch per axis.

### Spindle parameters

| # | Parameter | Value | Factory | Unit/choice |
|---|---|---|---|---|
| 188 | Spindle interface type | 0 | 0 | Analog |
| 189 | Spindle mapping axis | 3 | 3 | A axis |
| 190 | Spindle speed selection | 1 | 0 | Default **changed** |
| 191 | Default spindle speed | 12000 | 12000 | rpm |
| 192 | Maximum spindle speed | 24000 | 24000 | rpm |
| 193 | If Stop Spindle when pause | 1 | 1 | Yes |
| 194 | Spindle start-up waiting time | 2 | 0 | s **changed** |
| 195 | Spindle off waiting time | 2 | 0 | s **changed** |
| 412 | Automatic spindle shutdown after machining is completed | 1 | 0 | Yes **changed** |

`Spindle mapping axis = A axis` only matters if the interface type were
`PUL/DIR` (pulse/direction, like a stepper); since it's `Analog` (0–10V via
PWM, see [fpga-outputs.md](fpga-outputs.md)'s `ioctl 0x4627`; it comes out on **VSO, main port pin 2**, see [io-map.md](io-map.md)), this field is
inert — left at its default, not actually used.

### M output code parameters

| # | Parameter | Value | Factory | Unit/choice |
|---|---|---|---|---|
| 196 | delay time of M8/M9 | 1 | 1 | s |
| 197 | delay time of M10/M11 | 1 | 1 | s |

### Home parameters

| # | Parameter | Value | Factory | Unit/choice |
|---|---|---|---|---|
| 198 | HOME times | 1 | 1 | |
| 199 | X axis Home direction | 0 | 0 | Negative |
| 200 | Y axis Home direction | 0 | 0 | Negative |
| 201 | Z axis Home direction | 1 | 0 | Positive **changed** |
| 202 | A axis Home direction *(4th axis)* | 0 | 0 | Negative |
| 204 | X axis Home search speed | 1000 | 2000 | unit/min **changed** |
| 205 | Y axis Home search speed | 1000 | 2000 | unit/min **changed** |
| 206 | Z axis Home search speed | 1000 | 2000 | unit/min **changed** |
| 207 | A axis Home search speed *(4th axis)* | 700 | 2000 | deg/min **changed** |
| 209 | X axis Home positioning speed | 50 | 50 | unit/min |
| 210 | Y axis Home positioning speed | 50 | 50 | unit/min |
| 211 | Z axis Home positioning speed | 50 | 50 | unit/min |
| 212 | A axis Home positioning speed *(4th axis)* | 50 | 50 | deg/min |
| 214 | X axis Home offset | 925.2 | 0 | unit **changed** |
| 215 | Y axis Home offset | 600 | 0 | unit **changed** |
| 216 | Z axis Home offset | 0 | 0 | unit |
| 217 | A axis Home offset *(4th axis)* | 0 | 0 | deg |
| 219 | X axis back distance after Home | 50 | 10 | unit **changed** |
| 220 | Y axis back distance after Home | 50 | 10 | unit **changed** |
| 221 | Z axis back distance after Home | 15 | 10 | unit **changed** |
| 222 | A axis back distance after Home *(4th axis)* | 0 | 0 | deg |
| 224 | Home reminder after booting | 0 | 0 | No |
| 225 | Home sequence | 0 | 0 | ZXYA |
| 413 | Check HOME operation before machining | 0 | 0 | No |
| 800 | X axis first reference point(G28) | 30 | 30 | unit |
| 801 | Y axis first reference point(G28) | -30 | -30 | unit |
| 802 | Z axis first reference point(G28) | -50 | 10 | unit **changed** |
| 803 | A axis first reference point(G28) *(4th axis)* | 0 | 0 | unit |
| 805 | X axis second reference point(G30 P1) | 3 | 3 | unit |
| 806 | Y axis second reference point(G30 P1) | 3 | 3 | unit |
| 807 | Z axis second reference point(G30 P1) | 3 | 3 | unit |
| 808 | A axis second reference point(G30 P1) *(4th axis)* | 0 | 0 | unit |
| 810 | X axis third reference point(G30 P2) | 4 | 4 | unit |
| 811 | Y axis third reference point(G30 P2) | 4 | 4 | unit |
| 812 | Z axis third reference point(G30 P2) | 4 | 4 | unit |
| 813 | A axis third reference point(G30 P2) *(4th axis)* | 0 | 0 | unit |
| 815 | X axis fourth reference point(G30 P3) | 5 | 5 | unit |
| 816 | Y axis fourth reference point(G30 P3) | 5 | 5 | unit |
| 817 | Z axis fourth reference point(G30 P3) | 5 | 5 | unit |
| 818 | A axis fourth reference point(G30 P3) *(4th axis)* | 0 | 0 | unit |

### Software limit parameters

| # | Parameter | Value | Factory | Unit/choice |
|---|---|---|---|---|
| 234 | Enable software limit | 1 | 0 | Enable **changed** |
| 235 | Soft-limited postion value of X-- | 10 | -1200 | unit **changed** |
| 236 | Soft-limited postion value of Y-- | 10 | -600 | unit **changed** |
| 237 | Soft-limited postion value of Z-- | -300 | -300 | unit |
| 238 | Soft-limited postion value of A-- *(4th axis)* | 0 | 0 | deg |
| 240 | Soft-limited postion value of X++ | 915 | 1200 | unit **changed** |
| 241 | Soft-limited postion value of Y++ | 590 | 600 | unit **changed** |
| 242 | Soft-limited postion value of Z++ | -10 | 300 | unit **changed** |
| 243 | Soft-limited postion value of A++ *(4th axis)* | 0 | 0 | deg |

### MPG parameters

| # | Parameter | Value | Factory | Unit/choice |
|---|---|---|---|---|
| 61 | X axis MPG X1 speed | 100 | 100 | unit/min |
| 62 | Y axis MPG X1 speed | 100 | 100 | unit/min |
| 63 | Z axis MPG X1 speed | 100 | 100 | unit/min |
| 64 | A axis MPG X1 speed *(4th axis)* | 100 | 100 | deg/min |
| 66 | X axis MPG X10 speed | 500 | 500 | unit/min |
| 67 | Y axis MPG X10 speed | 500 | 500 | unit/min |
| 68 | Z axis MPG X10 speed | 500 | 500 | unit/min |
| 69 | A axis MPG X10 speed *(4th axis)* | 500 | 500 | deg/min |
| 71 | X axis MPG X10 speed | 1200 | 1500 | unit/min **changed** |
| 72 | Y axis MPG X10 speed | 1500 | 1500 | unit/min |
| 73 | Z axis MPG X10 speed | 1500 | 1500 | unit/min |
| 74 | A axis MPG X10 speed *(4th axis)* | 0 | 0 | deg/min |
| 76 | X axis MPG X1 Acc | 20 | 3 | unit/s2 **changed** |
| 77 | Y axis MPG X1 Acc | 20 | 3 | unit/s2 **changed** |
| 78 | Z axis MPG X1 Acc | 20 | 3 | unit/s2 **changed** |
| 79 | A axis MPG X1 Acc *(4th axis)* | 3 | 3 | deg/s2 |
| 81 | X axis MPG X10 Acc | 20 | 26 | unit/s2 **changed** |
| 82 | Y axis MPG X10 Acc | 20 | 26 | unit/s2 **changed** |
| 83 | Z axis MPG X10 Acc | 20 | 26 | unit/s2 **changed** |
| 84 | A axis MPG X10 Acc *(4th axis)* | 20 | 30 | deg/s2 **changed** |
| 86 | X axis MPG X100 Acc | 25 | 26 | unit/s2 **changed** |
| 87 | Y axis MPG X100 Acc | 25 | 26 | unit/s2 **changed** |
| 88 | Z axis MPG X100 Acc | 25 | 22 | unit/s2 **changed** |
| 89 | A axis MPG X100 Acc *(4th axis)* | 0 | 0 | deg/s2 |
| 245 | MPG Precision | 0.01 | 0.004 | **changed** |
| 246 | MPG motion direction | 1 | 0 | Positive **changed** |
| 247 | MPG control mode | 0 | 1 | Open **changed** |
| 248 | Enable the ESTOP signal on MPG | 1 | 1 | Enable |
| 249 | Electric level of ESTOP on MPG | 1 | 1 | High |

`#71-74` is a duplicate "X10 speed" block in `custom` itself (likely an
X100 block mislabeled upstream) — reproduced as-is from the source file
rather than silently corrected.

### External key parameters

| # | Parameter | Value | Factory | Unit/choice |
|---|---|---|---|---|
| 250 | Extended function key 1 function | 0 | 0 | Start |
| 251 | Extended function key 2 function | 1 | 1 | Pause |
| 252 | Extended function key 3 function | 11 | 11 | Disable |
| 253 | Extended function key 4 function | 11 | 11 | Disable |

### Backlash parameters

| # | Parameter | Value | Factory | Unit/choice |
|---|---|---|---|---|
| 254 | enable of X axis backlash | 0 | 0 | Disable |
| 255 | enable of Y axis backlash | 0 | 0 | Disable |
| 256 | enable of Z axis backlash | 0 | 0 | Disable |
| 257 | enable of A axis backlash *(4th axis)* | 0 | 0 | Disable |
| 259 | X axis backlash distance | 0.1 | 0.999 | unit **changed** |
| 260 | Y axis backlash distance | 0.1 | 0 | unit **changed** |
| 261 | Z axis backlash distance | 0 | 0 | unit |
| 262 | A axis backlash distance *(4th axis)* | 0 | 0 | deg |

### Tool offsets

All 16 tool slots (`H01-H16` length/wear, `D01-D16` diameter, `#264-311`)
are `0` on this machine — no tool table has been populated. Not reproduced
row-by-row here; see `decode_settings.py`'s output for the full list.

### System settings

| # | Parameter | Value | Factory | Unit/choice |
|---|---|---|---|---|
| 312 | language setting | 0 | 0 | Eng |
| 313 | Shift key operation mode | 1 | 1 | Menu Mode |
| 316 | Tool path paint enable | 1 | 1 | Enable |
| 317 | Tool path paint mode | 0 | 0 | Statue Mode |
| 318 | Tool path page display coordinates | 1 | 1 | Yes |
| 319 | X axis rotation angle in 3D mode | 10 | 10 | deg |
| 320 | Y axis rotation angle in 3D mode | 10 | 10 | deg |
| 321 | Z axis rotation angle in 3D mode | 10 | 10 | deg |
| 322 | Monitoring page disabled in machining | 0 | 0 | Yes |
| 323 | Enable of beep | 0 | 0 | Disable |
| 324 | Color reversal | 0 | 0 | No |
| 325 | Disable network functionality | 1 | 0 | No **changed** |
| 326 | Obtain IP address automatically | 1 | 1 | No |
| 327 | Local IP address | 192.168.1.200 | 192.168.2.5 | **changed** |
| 328 | Net mask | 255.255.255.0 | 255.255.255.0 | |
| 329 | Router IP address | 192.168.1.1 | 192.168.2.1 | **changed** |
| 330 | Shared host IP address | 192.168.1.65 | 192.168.2.8 | **changed** |
| 331 | Time zone settings | 0 | 0 | |
| 332 | Display real clock | 0 | 0 | Yes |

`Tool path paint mode = Statue Mode` (enum 0) is likely a translation
artifact for "Static Mode" (the other choices are "Line Mode" and "3D
Mode") — kept verbatim from the source string rather than corrected.

## What this means for the replacement app's defaults

These are now the factory/first-run defaults baked into the mockup and the
real app's config schema (see §5 of [architecture.md](architecture.md) and
the `Settings` screen in [operator-ux.md](operator-ux.md)), replacing the
placeholder numbers used before this machine's real `setting` file was
found:

| Setting | Default used |
|---|---|
| Units | mm |
| Steps/mm (X/Y/Z/A) | 2000 / 2000 / 2000 / 2000 |
| X DIR / pulse polarity | inverted relative to Y/Z/A (both confirmed) |
| Travel / soft limits | X 10–915, Y 10–590, Z −300…−10 (mm) |
| Home order | Z, X, Y, A |
| Home direction | X neg, Y neg, Z **pos**, A neg |
| Home search / positioning speed | 1000 / 50 mm/min |
| Limit+home wiring | one shared input per axis (X=IN02, Y=IN03, Z=IN04) — not separate +/− inputs |
| Probe | not wired (port unassigned) |
| External E-stop | wired, IN01 |
| Spindle | analog 0–10V, default 12 000 RPM, max 24 000 RPM |
| Z safe height (machining start) | 5 mm |
| Z workpiece safe height | 15 mm |
| Jog steps | 0.01 / 0.05 / 1 / 10 mm (CONT jog enabled) |
| Backlash compensation | off (values stored but not applied) |
| Network | DHCP off, static `192.168.1.200/24`, gateway `192.168.1.1` |

Tool table and work offsets (G54-59) are **not** sourced from this file —
they were empty/zero on the real machine — so the mockup's example tool
and WCS data stays clearly-labeled example data, not a claim about real
defaults.
