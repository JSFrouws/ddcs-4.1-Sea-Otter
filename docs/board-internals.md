# What is inside the DDCS V4.1 — the boards and their parts

_From photos of the opened unit, 2026-10-24 and 2026-10-31 (main board silkscreen
`DDCS_283V4.23`, CPU module `ICOR283_D44C V2.1`). Part numbers were read from the
packages; where a marking could not be resolved it says so. Nothing here was traced
electrically — the pin-level facts live in [connector-pinouts.md](connector-pinouts.md),
[fpga-motion.md](fpga-motion.md), [fpga-outputs.md](fpga-outputs.md) and
[mpg-pendant-io.md](mpg-pendant-io.md)._

## Overview

Two boards plus the display:

| Board | What it holds |
|---|---|
| **CPU module** `ICOR283_D44C V2.1` — a SODIMM-style plug-in card (gold edge connector into a DDR-SODIMM socket on the main board) | i.MX283 SoC, 64 MB DDR2, 4 GB eMMC, the **FPGA** with its own config flash, an RTC, the Ethernet PHY/USB region |
| **Main board** `DDCS_283V4.23` (`2509` date mark, `STM-5 94V-0` laminate) | power supply from the 24 V input, every connector (DB37 I/O, 2× DB15, DB9, 2× USB-A, RJ45, pluggable terminal block), the differential step/dir drivers, the opto-isolated inputs, the open-collector outputs, the 0–10 V spindle output, the keypad driver |
| Display | 7" panel on a 40-pin FPC, driven **directly by the SoC's LCD controller** (not by the TM1638, which only does keys and LEDs); a blue 10 kΩ trimmer (`103`) next to the FPC is the only adjustable part seen on that side |

Layout of the module (overview photo): eMMC at the top, i.MX283 in the middle, the Nanya
DDR2 to its right, the AGM FPGA (with the `4` sticker) below. The main board has no
processor of its own — only power, connectors, drivers, optos and the TM1638 on the panel side.

A CR2032 coin cell on the main board backs the RTC. The whole thing is a **generic ARM Linux
board with the motion-specific logic in the FPGA**, which is why the vendor app is just a
userspace program and `motiondev.ko` is a thin register window ([motiondev-ko.md](motiondev-ko.md)).

## CPU module

| Part | Marking | What it is |
|---|---|---|
| SoC | **NXP/Freescale `MCIMX283DVM4B`** (M06Z = mask revision, CTSY1725A = week 25 of 2017) | i.MX283: **ARM926EJ-S at 454 MHz, no FPU**, 16 KB/32 KB caches, LCD controller, 2× USB, 10/100 Ethernet MAC, PWM, timers/rotary decoder (TIMROT), SSP/SD, UARTs. MAPBGA-289. The `283` in every board name is this chip. This is the CPU the app's rendering budget and soft-float arithmetic are sized for ([app-architecture.md](app-architecture.md)). |
| RAM | **Nanya `NT5TU32M16DG-AC`** (1513 = week 13 of 2015, 3675270EP = lot) | **DDR2-800, 512 Mbit = 32M×16 = 64 MB**; one chip, 16-bit bus. Linux reports 58 MB. |
| Storage | **Toshiba/Kioxia `THGBMNG5D1LBAIL`** (VM8657 / 2109KAE = Toshiba internal and date codes) | **4 GB eMMC 5.0**, 153-ball. Holds the kernel, rootfs and `/ddcsv4` ([rootfs-findings.md](rootfs-findings.md)). |
| **FPGA** | **AGM `AG16KF256`** (AG16KF256 EKF79714, 2402-9403) | AGM Micro (Chinese) FPGA, **16K logic elements, 256-ball BGA** — the family is pin- and bitstream-tool-compatible with Altera Cyclone IV E (EP4CE15-class). It is the **step/direction pulse generator, position counters, MPG quadrature decoder, I/O latches and the register file** the app talks to over the 16-bit bus ([fpga-motion.md](fpga-motion.md)). |
| FPGA config | **Winbond `W25Q32JVSIQ`** (2223 = week 23 of 2022) | 32 Mbit (4 MB) SPI NOR flash next to the FPGA: the FPGA's own bitstream, loaded at power-up. This is the "flash chip the FPGA reads its gate configuration from" that the DDCSV1 reverse-engineering threads mention. Not reachable from Linux. |
| Small 8-pin next to the FPGA | `RC16 E12416 3PT` | Not identified with certainty: an 8-pin SOIC on the SPI/I²C side — most likely a small serial EEPROM or an oscillator. |
| RTC | `CF8563` (J2 03) | PCF8563-compatible I²C real-time clock (a Chinese second source), with a 32.768 kHz crystal and the CR2032 on the main board. |
| Power on the module | `150` shielded inductor + SOT-23-5 regulator, two 0603 inductors | the module's own buck converter(s) for the SoC core/IO rails |
| Near the RJ45 (main board side, under the module edge) | 32-pin QFN `3AA7MU3 / 8304ES0`, SOIC-20 | Ethernet PHY + magnetics side for the i.MX28 MAC (marking not resolved to a vendor part) |
| Sticker `4` on the module's largest BGA | — | a factory test/bin label, nothing functional |

## Main board — power

24 V in on the pluggable terminal block, local rails made on the board:

| Part | Marking | Role |
|---|---|---|
| **MPS `MP4462DN`** (NA775788) | SOIC-8 buck | 3.5 A, up to 36 V in, synchronous step-down: 24 V → 5 V (the big `150` = 15 µH shielded inductor, `SS310` 3 A/100 V Schottky diodes and a `150` = 15 µF tantalum sit around it) |
| `2R2` inductor + SOT-23-5 `IAEUK` | small buck | 5 V → 3.3 V (or 1.8 V) for the logic |
| `LM393` (TI, 93M DN24YX) | dual comparator | supply/undervoltage sensing — this is the obvious source of the **power-fail interrupt** that `motiondev.ko` registers (`gpio_int`, [motiondev-ko.md](motiondev-ko.md)) so positions can be saved on a dying supply |
| `MM` SMB diode | — | input protection / reverse polarity |
| Two `JF` SMD parts in fuse outlines | resettable fuses | on the 5 V / COM+ side of the I/O (the "I/O power" terminals) |
| 8-pin DIP in a socket near the terminal block | not legible | the only socketed part — most likely a small EEPROM or an isolated DC/DC for the I/O side |

## Main board — the I/O that the app drives

| Function | Parts seen | Notes for the app |
|---|---|---|
| **Step / direction outputs** (XP±/XD± … AP±/AD±, four axes) | **TI `AM26LS31C`** quad differential line drivers (RS-422), two SOIC-16 visible next to the DB15 | Each chip drives 4 differential pairs; two chips = the 8 pairs of four axes. **Fully differential ±3 V-class outputs** — a drive wants either the pair or one side vs. COM. **The dead X DIR output** ([hardware-setup.md](hardware-setup.md): swings −3 V / +0.77 V unloaded) is one channel of one of these chips; the AM26LS31 is a 1-euro part in SOIC-16 and could be replaced. The FPGA's channel → connector mapping that `axis.*.output` works around is upstream of these drivers. |
| **Inputs IN01–IN13, limits, home, probe, pendant E-stop** | rows of **Lite-On `LTV-247`** quad and `LTV-227` dual optocouplers, each input with `103` (10 kΩ), `302` (3 kΩ) / `512` (5.1 kΩ) resistors and a ceramic cap | Opto-isolated, COM+/COM− referenced, which is why nothing reads without 24 V on COM+ (the desk-test trap in hardware-setup.md). The RC in front of each opto is the hardware debounce — a few ms, which is part of the limit-detection latency measured in fpga-motion.md. |
| **Outputs OUT1–OUT3, spindle M3/M8/M10** | **`ULN2003A`** Darlington array (SOIC-16, `N2003A`) | Open-collector **sinking** outputs: "on" pulls the pin to COM−. This is the hardware reason an output bit = 1 means pulled down in [fpga-outputs.md](fpga-outputs.md), and why the drive-enable setting has a level option. 500 mA/channel rating. |
| **Spindle 0–10 V (VSO)** | **`GP8101`** (Guestgood, SOIC-8, `300100`) + ST `LM358` op-amp | GP8101 is a **PWM-to-0–10 V DAC**: the SoC/FPGA feeds it a PWM, the LM358 buffers/scales the analog output. So spindle speed resolution is the PWM resolution, not a true DAC. The `BE` SOT-23 and `68X` parts around it are the output stage. |
| **Protection on the I/O lines** | 16 SMA-package diodes marked `WK` with a four-leaf logo, in two rows of 8 behind the DB37 | one per I/O line: clamp/TVS or Schottky protection — exact type not resolved from the marking |
| **Keypad + LEDs** | **`TM1638`** (U2, Titan Micro, 2436YBQ0018MH = date/lot) on the front-panel board | LED-display/key-scan driver: up to 8 digits × 10 segments and a 3×8 key matrix over a 3-wire bus (STB/CLK/DIO); the app bit-bangs it over GPIO via `/dev/mem` ([app-architecture.md](app-architecture.md)). **Not the screen driver.** `R2–R4` and `C2` are its pull-ups and decoupling. |
| **MPG pendant** (DB15 "MPG") | differential inputs into the FPGA | the quadrature A/B pairs are decoded in the FPGA (`0x0E` count register, [mpg-pendant-io.md](mpg-pendant-io.md)), not in the SoC's TIMROT. |

## What this means for the firmware

- **Pulse generation is entirely in the FPGA.** The ARM only writes step counts and rates
  into registers; it cannot modulate a pulse once a burst is running. Everything in
  [fpga-motion.md](fpga-motion.md) about idle-only bursts follows from that. True
  continuous motion needs the FPGA's coordinated block interface decoded — there is no
  CPU-side fast path.
- **16K-LE FPGA** is small but enough: four pulse generators with 32-bit counters, the
  quadrature decoder, 16 inputs, a handful of outputs and a register file fit easily.
  The bitstream is in the Winbond flash and is not touched by any of this work.
- **No FPU, 454 MHz, one core**: the single-threaded loop, soft-float budget and the
  rendering cache are not optional ([app-architecture.md](app-architecture.md)).
- **Outputs sink, inputs are opto-coupled, step/dir is RS-422** — the three facts to
  remember when wiring anything to it.

## Gaps

Not resolved from the photos: the exact Ethernet PHY, the `RC16` 8-pin part, the socketed
DIP-8, the `WK` diode type, and which side of the module the PWM for the GP8101 comes from
(SoC PWM or FPGA). None of them affects the app.
