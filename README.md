# DDCS V4.1 "Sea Otter" - open replacement software for the DDCS V4.1 CNC controller

**Pandora** ([app/](app/README.md)) is a complete replacement for the controller's vendor
software: G-code (incl. drilling cycles), lookahead motion planning, homing, touch-off,
work offsets, multi-part jobs, resume-from-line, TryCut and Simulate, overrides, macros,
the panel UI on the 1024x600 LCD with the 18 keys and the MPG pendant, and an Ethernet API.
It runs on the controller (static ARMv5 binary) and on a PC against an FPGA simulator.

- **Operators:** [docs/user-manual.md](docs/user-manual.md) ([PDF](docs/user-manual.pdf))
- **Installing:** from a USB stick, [docs/usb-install.md](docs/usb-install.md) ·
  SSH with a password: [docs/ssh-setup.md](docs/ssh-setup.md)
- **API:** [docs/api-reference.md](docs/api-reference.md)
- **How it works:** [docs/app-architecture.md](docs/app-architecture.md),
  [docs/fpga-motion.md](docs/fpga-motion.md), [docs/motion-planner.md](docs/motion-planner.md)
- **All findings about the hardware:** [docs/README.md](docs/README.md)

Status (2026-10-18): step-exact on the real controller's FPGA with real CAM programs
(up to 64 000 lines); **not yet tested with motors or real limit/home switches.**

This repository holds `app/` and `docs/`. The vendor's firmware files are not included.

---

## Background: the motion driver port


A port of the open-source [Pandora-CNC](https://github.com/Pandora-CNC)
`motiondev` driver (originally for the DDCSV1.1 / Nuvoton W55FA93) to the
**DDCS V4.1**, whose main board is a **Freescale i.MX28 (ARM926, ARMv5)**
running **Linux 2.6.35.3**.

Everything here was derived by disassembling the vendor firmware package
`ddcsv4(2025-04-04)` — specifically `motiondev.ko` (driver, GPL, build
string `20210830`) and `ddcsv4.out` (the application). No vendor source was
used; the driver is GPL like the original.

> **Status: compiles and is vermagic-compatible with the controller. It has
> NOT been run on hardware.** Treat it as a reverse-engineering scaffold and
> a trace tool, not a drop-in replacement yet. See *Unknowns* below.

## What this gives you

1. **A compatible `/dev/motion`** implementing the vendor ioctl ABI
   (`0x4620`–`0x4628`), so in principle `ddcsv4.out` can run on top of it.
2. **A second node `/dev/motion-trace`** with a per-register trace + bypass
   engine (Pandora's idea, extended to cover the handwheel, keypad, LEDs,
   spindle PWM and the power-fail interrupt). This is the real point: it lets
   you **record exactly what the vendor app writes to the FPGA** while you
   jog and cut, which is the information you need to write your own control
   software.
3. **`mtrace`**, a small static ARM tool to drive the trace node and do
   one-off register reads/writes.

## How the hardware was decoded (evidence)

From `motiondev.ko`:

| Finding | Evidence |
|---|---|
| SoC = i.MX28 | Imports `mxs_request_pin/strength/voltage`, `clk_get("pwm")`; register bases `0xF006_4000` (PWM), `0xF006_8000` (TIMROT), pin-mux at `0x..18000` — the mxs/i.MX28 map. The name `w55fa93-motion` is a left-over label from the V1.1 ancestor. |
| FPGA bus = bit-banged GPIO | `ADDROUT/DATAOUT/DATAIN/WR_DATA/RD_DATA` scatter address/data across GPIO banks 1/2/3 and toggle `gpio 17` (WR) / `gpio 20` (RD). |
| Bus bit order | The shift/mask constants `0x001F07FF`, `0x00F00000`, `0x1C400000` in `ADDROUT`/`WR_DATA`/`DATAIN`. |
| Keypad/LED = TM1638 | `TM1638_Read/Write`, `Write_COM`, `Write_Nixie_DATA`, `Read_Nixie_DATA` bit-bang `gpio 49/50/51` (STB/DIO/CLK) with the TM1638 `0x44` / `0xC0+addr` / `0x8F` / `0x42` command set. |
| ioctl map | `motion_ioctl` jump table: `0x4620` write, `0x4621` read, `0x4622` 18-word block → regs `0x50..0x60` + latch `0x66`, `0x4623/24` beep, `0x4625` power status, `0x4626/28` LED/key, `0x4627` PWM. |
| Spindle PWM = channel 6 | `WR`-style writes to PWM period `0x005A0BB7 / 0x005B0BB7 / 0x005F0BB7` (off / run / full). |
| Power-fail IRQ | `motion_open` requests a threaded IRQ on `gpio 85` and `kill_fasync`es the app on the rising edge. |

The file-by-file register/pin map is in `driver/motiondev_hw.h`.

## Building the driver

You need the **matching kernel source**: Linux **2.6.35.3**, configured
ARMv5 + `PREEMPT` + `MODVERSIONS` + `MODULE_UNLOAD`, so the vermagic comes
out as:

```
2.6.35.3-571-gcca29a0 preempt mod_unload modversions ARMv5
```

This repo's driver already builds to that exact vermagic. Then:

```sh
make -C driver \
     KDIR=/path/to/linux-2.6.35 \
     ARCH=arm CROSS_COMPILE=arm-linux-gnueabi-
```

> **Symbol-version (CRC) caveat.** Module loading on this kernel checks a CRC
> per imported symbol (`modversions`). Those CRCs come from a *full* kernel
> build (`Module.symvers` / `vmlinux`), which needs Digital Dream's exact
> `.config` and the i.MX28 BSP. Until you have that, the `.ko` will have zero
> CRCs and the controller kernel will reject it unless you load with
> `insmod -f` / `modprobe --force-modversions`. The cleanest path is to pull
> the real `Module.symvers` off the device (or the BSP) and rebuild.

## Getting onto the controller (recovery first!)

Before replacing anything, **establish a recovery path so a bad module can't
brick the box:**

- Find the board's **debug UART** (serial console). The i.MX28 ROM and
  U-Boot print there; it gives you a root shell independent of `ddcsv4.out`.
- Confirm the **i.MX28 USB recovery (`RECOVERY`/`USB-boot`) mode** via the
  boot-mode pins. With `mxsldr`/`uuc` you can always reload a known-good
  kernel+rootfs over USB even if flash is wiped.

With a console you can `cp` the stock `ddcsv4` folder somewhere safe, then
`rmmod motiondev; insmod motiondrv.ko` and roll back instantly if needed.

## Recommended workflow (what this is actually for)

1. Build + deploy `mtrace` and (optionally) this driver with trace enabled.
2. `mtrace reset; mtrace on`, then make the stock app jog each axis a known
   distance, change feed, start/stop spindle, turn the handwheel one detent.
3. `mtrace dump > capture.txt; mtrace off`.
4. From the capture, work out the FPGA register meaning: step-rate/velocity
   words (regs `0x50..0x60` in the block), position read-back, I/O, and the
   MPG count. That register map is the one missing piece between here and a
   full from-scratch controller.

## Unknowns / assumptions (verify on hardware)

- **Block word order** inside ioctl `0x4622`. The V1.1 driver reordered the
  18 words (`pbuf[0,1,4,5,6,7,8,9,2,3,10..15]`) and double-latched; the V4.1
  binary writes `0x50..0x60` straight through. This port does the
  straight-through V4.1 form — confirm against a capture.
- **DIGCTL microsecond counter offset** (`0x0C0`) for trace timestamps.
- **TIMROT rotary-decoder config word** for the MPG (`0x00001054`): taken
  from the binary but the exact SELECT fields should be checked.
- **TM1638 read glitch-filter**: the vendor does a 2-of-3 vote; replicated
  but not timing-verified.
- Exact `Module.symvers` CRCs (see caveat above).

## Layout

```
driver/   motiondev.c        main driver, ioctl ABI + trace engine
          motiondev_lld.c    i.MX28 FPGA bus + TM1638 bit-bang
          motiondev_hw.h     all recovered addresses/pins/regs (the map)
          motiondev.h        trace record + control-op ABI
          Makefile
tools/    mtrace.c           trace/one-shot-IO userspace tool
```

## License

GPL-2.0, following the vendor `motiondev.ko` (GPL) and Pandora-CNC.
