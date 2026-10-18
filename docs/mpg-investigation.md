# MPG (handwheel) count — RESOLVED 2026-10-16

> **Answer: `FPGA_REG_MPG_COUNT = 0x0E`**, a plain FPGA bus register read the same way as
> any other (`MOTION_IOC_READ`, addr `0x0E`). Confirmed live on real hardware: watched it
> change in real time while the handwheel was turned, and go flat exactly when rotation
> paused. Now in [../driver/motiondev_hw.h](../driver/motiondev_hw.h) and
> [../driver/motiondev.c](../driver/motiondev.c)'s `mpg_count()`. The rest of this doc is
> the investigation trail that got there, kept for context.

# (history) MPG (handwheel) count — inconclusive, needs a retest with the real app

_2026-10-16. `ioctl_probe` ([mockup/ioctl-probe/](../mockup/ioctl-probe/)) read
`MOTION_IOC_MPG_COUNT` as 0 both before and after turning the connected handwheel, while
`recovery_shell` (not the real `ddcsv4.out`) was the running process. Inconclusive -
written up here so the retest isn't forgotten._

## What was ruled out

- **Not a missing app-level GPIO power-enable step.** Searched `ddcsv4.out` for any
  `gpio`/`/sys/class`/`power`/`mpg`/`encoder`/`rotary` strings beyond what's already in
  `motiondev.ko` - found nothing. The app has no separate hardware-enable logic; it only
  talks to the hardware through `/dev/motion` ioctls, same as our own driver.
- **`motiondev.ko` (the vendor's) is loaded and was loaded the whole time** (`lsmod`
  confirmed `motiondev` live), and its own `init_module` configures `TIMROT_ROTCTRL`
  and the `PWM3`/`PWM4` pin routing unconditionally at module-load time - this doesn't
  depend on `ddcsv4.out` running at all.

## Resolved: raw hardware register also reads 0 - likely physical, not software

First tried `/dev/mem`/`dd` with a plain `read()`: `dd: /dev/mem: Bad address` - this
kernel doesn't serve a plain `read()` on an MMIO physical range through `/dev/mem` the
way it does for ordinary RAM. `/dev/kmem` at the driver's assumed virtual address
(`0xF0068010`, per `MX28_IO()`) also failed (`No such device or address`) - unlike the
earlier successful `/proc/kallsyms` CRC read, an `ioremap()`'d peripheral region is a
dynamic vmalloc-area mapping that `/dev/kmem`'s simple linear-offset read doesn't serve
(this also means `MX28_IO()`'s static-mapping assumption still isn't confirmed or
refuted - the ENXIO is about `/dev/kmem`'s own limits, not the address).

**The correct way to access MMIO from userspace is `mmap()` on `/dev/mem`, not
`read()`.** Built [../mockup/ioctl-probe/regpeek.c](../mockup/ioctl-probe/regpeek.c) to
do exactly that - and it worked:

```
$ /ddcsv4/regpeek 0x80068000   # TIMROT_ROTCTRL
[0] 0x80068000 = 0x3e001054

$ /ddcsv4/regpeek 0x80068010 5 2   # TIMROT_ROTCOUNT, 5 reads, 2s apart
[0..4] 0x80068010 = 0x00000000   (all five, completely stable over 10s)
```

**`TIMROT_ROTCTRL`'s low 16 bits read back `0x1054` - an exact match for our
`MPG_ROTCTRL_VALUE` assumption in `motiondev_hw.h`.** (The upper bits, `0x3e00`, are
presumably live status/readback flags rather than configuration - no iMX28 TIMROT
bitfield reference was consulted to decode them further.) So the register
**configuration is confirmed correct** independent of ioctls, the vendor driver, or
which `ddcsv4.out` is running.

**`TIMROT_ROTCOUNT` reads exactly 0, completely stable across 5 reads over 10 seconds** -
not just "happened to read 0 once," genuinely static. Since the counter register itself
never moves at the hardware level, **this points away from a software/ABI problem and
toward the physical connection**: either the handwheel on the desk isn't actually wired
to the PWM3/PWM4 pins this driver assumes it uses, or it's on a different connector
entirely. (See [hardware-setup.md](hardware-setup.md) - the main I/O connector is mostly
unplugged in the current desk setup, and it was never confirmed which physical connector
the handwheel actually uses on this board.)

## What actually happened next (2026-10-16, after a power-cycle and real-app test)

The wiring-problem hypothesis above turned out to be **wrong** - the user pointed out
that the dial visibly moved a menu cursor in the live app, so the signal clearly *was*
reaching the device somewhere. Good catch; it redirected the whole investigation.

Hardware for reference: a genuine purchased pendant, **"Handwheel handling unit MPG
100ppr", model MBL-100P-5L** - 100 pulses/rev. Initially described (from memory) as having
four signal lines labeled `TXD1 TXD2 RXD1 RXD2` + GND + 5V - this turned out to be a
mix-up with a *different* connector's silkscreen labels (the unused "HMI/RS232" port, see
[connector-pinouts.md](connector-pinouts.md)). The pendant is actually plugged into the
board's **dedicated 15-pin "MPG" connector**, confirmed by photo, which carries genuine
**differential quadrature**: `WHA+/WHA-` and `WHB+/WHB-` (real A/B pulse pairs) + GND +
5V. Either way the "differential, decoded directly by the FPGA rather than the SoC" hunch
turned out right - this just nails down *which* differential pair.

### Ruling out the official ioctl too

With the real `ddcsv4.out` stopped (user's permission, to free up `/dev/motion` - the
driver only allows one opener, which is also why `ioctl_probe` hung earlier when tried
alongside the running app), a purpose-built live logger
([../mockup/ioctl-probe/keylog.c](../mockup/ioctl-probe/keylog.c)) polled
`MOTION_IOC_KEY` and `MOTION_IOC_MPG_COUNT` through the vendor's own loaded driver for
60s while every physical control was operated. **Zero key events, `MPG_COUNT` never left
0.** This cleanly rules out `ioctl 0x4625` as the real MPG-reading path on this V4.1
firmware (matching the README's old, previously-unresolved "power status" vs "MPG_COUNT"
naming discrepancy inherited from the V1.1 driver).

Also found while the app was stopped: `/dev/input/event0`, **"MXS touchscreen"** - the
on-screen buttons are ordinary touch events through the standard Linux input subsystem,
unrelated to the FPGA/keypad path entirely.

### Finding the real register: a direct FPGA bus read, done right

Built [../mockup/ioctl-probe/fpgabus.c](../mockup/ioctl-probe/fpgabus.c): a userspace
tool replicating `motiondev_lld_read_data()` via `mmap()` on `/dev/mem`, to read any FPGA
register directly - independent of `/dev/motion` and the one-opener limit.

First attempt was unreliable (different "random" registers showing values on every
re-run, even with the app stopped and zero bus contention). Root cause: **no settle
delay between bus phases**. The kernel driver's `__raw_writel()`/`gpio_set_value()`
calls have enough inherent call overhead that the vendor code never needed an explicit
delay; a tight userspace loop doing the same raw stores back-to-back outran the FPGA's
real address/data setup and hold time. Fixed by adding a small delay (dummy register
reads) between each bus phase (address out → data-direction-in → RD strobe → sample →
RD low → direction-out). Validated by rereading the already-known-correct
`FPGA_REG_VERSION` (`0x41`) eight times in a row - all eight now matched `0x0809`,
where before the fix they would have been noise.

With the tool trustworthy, a clean **before/after full register sweep** (`0x00-0xFF`,
app stopped so no contention) showed only **one** address differing between the two
scans: **`0x0E`**, which dropped by exactly `-196` (`-4 × 49` - the familiar x4
quadrature-count signature). A synchronized live watch (`fpgabus 0x0e 15 1`, run exactly
as the user started turning the dial) confirmed it beyond doubt: the value changed
continuously while rotating and went completely flat the moment rotation paused.

### Conclusion

**`FPGA_REG_MPG_COUNT = 0x0E`** is the live handwheel count, read like any other FPGA
register via `MOTION_IOC_READ`. It's a 16-bit, free-running (wrapping) counter that
decrements for the rotation direction tested. The SoC's TIMROT peripheral
(`TIMROT_ROTCOUNT`) is configured at init (matching the vendor binary exactly) but
appears otherwise unused for this - the FPGA does its own quadrature decode on this
board, unlike the V1.1 ancestor hardware the original driver assumption came from.

Updated: [../driver/motiondev_hw.h](../driver/motiondev_hw.h) (new
`FPGA_REG_MPG_COUNT` define, with full history in its comment) and
[../driver/motiondev.c](../driver/motiondev.c)'s `mpg_count()` (now reads the FPGA
register instead of TIMROT).

### Lesson for next time

Direct hardware bit-banging from userspace needs explicit settle/delay logic that a
kernel driver's normal call overhead provides "for free" - don't trust the first result
from a fast, tight-loop reimplementation of a slow bus protocol without validating it
against an already-known-correct value first.
