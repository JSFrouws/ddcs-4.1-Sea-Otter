# motiondev.ko on the unit vs. our assumptions

_2026-10-15. Source: `SYSDISK/motiondev.ko` from the unit, backed up in
[raw/sysdisk-2026-10-15/](raw/sysdisk-2026-10-15/). Tool: [../tools/ko_inspect.py](../tools/ko_inspect.py)
(stdlib ELF reader; there's no binutils on the dev PC). Full dumps:
[raw/2026-10-15_motiondev-ko-device.txt](raw/2026-10-15_motiondev-ko-device.txt),
[raw/2026-10-15_motiondrv-ko-ours.txt](raw/2026-10-15_motiondrv-ko-ours.txt)._

| | Unit's `motiondev.ko` | Our `driver/motiondrv.ko` |
|---|---|---|
| SHA-256 | `6e309c63…7d684a4eee` | `ae2ffa8d…7f0eeca4` |
| Size | 14231 B | — |
| Build string | `motion driver has been initialized successfully!20210830` | — |
| vermagic | `2.6.35.3-571-gcca29a0 preempt mod_unload modversions ARMv5` | **identical** |
| `__versions` | **35 real CRCs** | **0 entries**: will fail the modversions check |
| Module params | `major_no`, `minor_no` (int) | none |

**The unit runs the same driver build (`20210830`) that the README and `motiondev_hw.h`
were derived from.** The reverse engineering applies to this unit as-is.

## Assumptions checked against the unit's binary

| Assumption (motiondev_hw.h / README) | Found in device .ko | Status |
|---|---|---|
| PINCTRL base `0xF0018000` | literal, 8x in `.text` (all bus/TM1638 helpers) | confirmed |
| PWM base `0xF0064000` | literal in `motion_ioctl` pool + `init_module` | confirmed |
| TIMROT base `0xF0068000` | literal in `motion_ioctl` pool + `init_module` | confirmed |
| DIGCTL `0xF001C000` (trace timestamps) | **not used by vendor** | our own addition. Offset `0xC0` still an ASSUMPTION |
| `DATA_BANK2_MASK 0x001F07FF` | literal 5x | confirmed |
| `ADDR_BANK1_MASK 0x00F00000` | `AND rX, #0xf00000` in `ADDROUT` (+0x24, +0x34) | confirmed (immediate, not literal) |
| `ADDR_BANK3_MASK 0x1C400000` | `AND r1, #0x1c400000` in `ADDROUT` (+0x38) | confirmed |
| Spindle OFF `0x005A0BB7` / RUN `0x005B0BB7` | literals | confirmed |
| Spindle FULL `0x005F0BB7` | built as `0xBB7` + `ADD #0x5f0000` (+0x648) | confirmed |
| MPG `ROTCTRL 0x00001054` | literal in `init_module` | confirmed |
| Pin ids PWM3/4/6 = `0x03000014/15/17` | literals in `init_module` | confirmed |
| GPIO WR=17, RD=20, INT=85, BEEP=125, LED 49/50/51 | `MOV r0,#0x11/#0x14/#0x55/#0x7d/#0x31..0x33` before gpio calls | confirmed |
| FPGA regs 0x3B (keys ext), 0x41 (version), 0x50 (block), 0x66 (latch) | `MOV r0,#0x3b/#0x41/#0x50/#0x66` | confirmed |
| ioctl `0x4620..0x4628` | `SUB r2,#0x4600` … `CMP r2,#8` jump table (9 cases) | confirmed |
| Pin names | strings `ARM_WR ARM_RD ARM_INT ARM_ADDR0-7 ARM_D0-15 BEEP LED_SS LED_SCK LED_DIO PWM3 PWM4 PWM6` | confirmed: 8 address + 16 data lines |
| Power-fail IRQ name | string `gpio_int`, imports `__gpio_to_irq`, `request_threaded_irq` | confirmed |
| FPGA version printk | `motion ver:%0x\n` | confirmed |

## Open questions and discrepancies

1. **ioctl `0x4625`**: the README calls it "power status", but `motiondev_hw.h` names it
   `MOTION_IOC_MPG_COUNT` (TIMROT ROTCOUNT). The TIMROT base is in the ioctl's literal pool, so
   *some* ioctl reads the encoder. Which case does what still needs a per-case disassembly.
2. The vendor imports `disable_irq`, `down_trylock`, `param_get/set_int`. Ours doesn't, which is
   fine. It means behaviour differs a little (module params, IRQ handling in close).

## The modversions problem — fixed (2026-10-15)

The unit's module carries the **real CRCs** for every kernel symbol it imports, including
`module_layout = 0x405fe887`. Plan: make our imports a **subset** of the vendor's, then
reuse the vendor CRCs. Done:

**1. Source reduced to the vendor's symbol set** ([../driver/motiondev.c](../driver/motiondev.c)):

| Was (no vendor CRC) | Now (vendor exports it) | Change |
|---|---|---|
| `down_write`/`up_write` (rwsem) | `down`/`up` (semaphore) | `DECLARE_RWSEM` → `struct semaphore` + `sema_init` |
| `vmalloc`/`vfree` | none | trace ring is now a static 512 KiB `.bss` buffer |
| `preempt_schedule` | — | an artifact of the earlier build against generic headers; the vendor uses the same spinlocks without it, so it should not reappear when built against the real 2.6.35.3 tree |

After the change our expected imports are **30, all present in the vendor's 35** (verified).

**2. `Module.symvers` generated from the vendor module**
([../driver/Module.symvers](../driver/Module.symvers), via
[../tools/gen_symvers.py](../tools/gen_symvers.py)). The Makefile feeds it to modpost through
`KBUILD_EXTRA_SYMBOLS`, so a rebuild stamps the correct CRCs **without a full vmlinux build**.

**3. Fallback patcher** ([../tools/patch_ko_crc.py](../tools/patch_ko_crc.py)) stamps the CRCs
straight into a built `.ko`'s `__versions` table. Verified by round-trip: zeroing the vendor
module's CRCs and patching them back from `Module.symvers` reproduces the vendor `.ko`
**byte-for-byte** (sha256 `6e309c63…4a4eee`).

**Still required:** a rebuild on a Linux host with the ARM toolchain + 2.6.35.3 headers — it
can't be done on the Windows dev PC. The current committed `motiondrv.ko` has an **empty
`__versions`** (built without stamping) and must be rebuilt from the updated source.

**Update, 2026-10-16: done.** With a live shell ([rootfs-findings.md](rootfs-findings.md)),
`/proc/kallsyms` gave the address of `__kcrctab_preempt_schedule` (`c0608b3c`), and
`dd if=/dev/kmem bs=4 skip=$((0xc0608b3c/4)) count=1 | od -x` read the actual CRC value
straight out of kernel memory: **`0x43b0c9c3`**, now appended to
[../driver/Module.symvers](../driver/Module.symvers) (36 entries). So even if
`preempt_schedule` does reappear in a rebuild, it's covered - every symbol our driver could
plausibly import now has a known-correct CRC, closing this item completely without ever
needing the vendor's BSP or a full kernel build.

## How it's loaded

**Confirmed** from `/etc/init.d/rcS` ([rootfs-findings.md](rootfs-findings.md)):
`/sbin/insmod /ddcsv4/motiondev.ko`, then `cd /ddcsv4/ && ./ddcsv4.out &`. Also confirmed:
**the real way to update files on `SYSDISK` is a USB stick** - the boot script
unconditionally runs `cp /udisk-sda1/ddcsv4/* /ddcsv4/` before any of this, which is a far
simpler deployment path for the rebuilt `motiondrv.ko` than SMB (no running-file
ETXTBSY/STATUS_CANNOT_DELETE dance needed, since nothing has started yet at that point in
boot).
