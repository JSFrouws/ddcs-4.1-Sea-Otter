# Our driver vs. the vendor's `motiondev.ko`

_2026-10-15. Compares [../driver/motiondev.c](../driver/motiondev.c),
[../driver/motiondev_lld.c](../driver/motiondev_lld.c),
[../driver/motiondev_hw.h](../driver/motiondev_hw.h) and
[../driver/motiondev.h](../driver/motiondev.h) against the vendor's on-device
`motiondev.ko`, as disassembled in [motiondev-ko.md](motiondev-ko.md), and
against `ddcsv4.out`'s use of `/dev/motion` in [app-analysis.md](app-analysis.md)._

## 1. ioctl ABI — functionally compatible

Both drivers expose the same `0x4620..0x4628` command space and the same
arg shapes (confirmed in motiondev-ko.md's disassembly of the jump table):

| ioctl | Vendor | Ours |
|---|---|---|
| `0x4620` WRITE | `{addr,data}` → FPGA bus write | identical (`data_write`) |
| `0x4621` READ | `{addr,data}` in/out | identical (`data_read`) |
| `0x4622` BLOCK | 18 words → regs `0x50..0x60` + latch `0x66` | identical target registers; see word-order caveat below |
| `0x4623/0x4624` BEEP on/off | GPIO toggle | identical (`GPIO_BEEP` = gpio 125) |
| `0x4625` MPG/power | TIMROT literal in the ioctl's pool, exact case unresolved (open question #1 in motiondev-ko.md) | named `MOTION_IOC_MPG_COUNT`, reads `TIMROT_ROTCOUNT` directly — **an assumption**, not yet confirmed against a live capture to be ioctl `0x4625` specifically vs. "power status" |
| `0x4626` LED | TM1638 digit/segment write | identical (`motiondev_lld_led_write`) |
| `0x4627` SPINDLE | PWM channel 6 period word | identical three-state model (off/run/full) |
| `0x4628` KEY | TM1638 + FPGA reg `0x3B` scan → event code | identical scan/FIFO logic (see §3) |

So any userspace that only calls these nine ioctls (i.e. `ddcsv4.out`) should
not be able to tell the two drivers apart — **by construction**, since
`motiondev_lld.c`'s header says it replicates "bus timing, bit order and the
key FIFO behaviour... exactly". The one open functional question is whether
`0x4625` is really MPG-count on the V4.1 as well as the V1.1 ancestor; see
motiondev-ko.md's open question #1.

## 2. GPIO / FPGA register handling — same bus, different SoC glue

Structurally the two drivers can't be line-for-line identical because they
sit on different SoCs (vendor: i.MX28 glue reused from a Nuvoton W55FA93
driver per the README's "w55fa93-motion" leftover label; ours: written
directly against i.MX28/mxs). But the **bus protocol itself** — address on
GPIO banks 1/3, data on GPIO bank 2, `ARM_WR`/`ARM_RD` strobes — is the same
in both, because both were derived from the same physical FPGA, and every
mask/offset in `motiondev_hw.h` is marked "confirmed" against the device
`.ko` in motiondev-ko.md (PINCTRL base, PWM base, TIMROT base, bank masks,
spindle period words, GPIO numbers, FPGA register numbers, the ioctl jump
table itself). The **only unconfirmed hardware constant** carried into our
port is `DIGCTL_MICROSECONDS` at offset `0xC0` — the vendor driver **does
not use it at all** (it has no trace timestamps), so there was nothing to
confirm it against; it's our own addition for `/dev/motion-trace` and stays
an assumption per the README's "Unknowns" section.

## 3. Spindle / MPG / keypad / LED behaviour

- **Spindle**: identical three-level PWM period model (`SPINDLE_PERIOD_OFF /
  _RUN / _FULL` on channel 6), all three literal words confirmed against the
  device binary.
- **MPG**: identical TIMROT rotary-decoder read; `MPG_ROTCTRL_VALUE
  0x00001054` confirmed as a literal in the vendor's `init_module`.
- **Keypad**: `motiondev_lld.c`'s key-scan state machine (5 scan bytes, 4 from
  TM1638 + 1 from FPGA reg `0x3B`, edge-detect into a 4-deep FIFO, code =
  `byte*8+bit+1`, bit 7 = pressed) is an intentional byte-for-byte port of the
  vendor's logic, including the "vendor quirk kept on purpose" 2-of-3 glitch
  filter on the TM1638 read and the doubled dummy clock pulse — the comments
  in the source say this was deliberate, not incidental.
- **LEDs**: `motiondev_lld_led_write` issues the same TM1638 command
  sequence (`0x44` fixed-address mode, `0xC0+digit` address byte, segment
  byte, `0x8F` display-on) the vendor uses.

Net effect: peripheral behaviour is a **straight reimplementation**, not a
reinterpretation — the one area with no vendor equivalent to match against is
the trace engine itself (§4).

## 4. `/dev/motion-trace` — pure addition, no vendor equivalent

This is the single largest structural difference. The vendor `motiondev.ko`
has no trace/bypass facility of any kind — it is a plain pass-through driver.
Everything in `motiondev.c`'s "trace engine" block (`trace_rec`, the 64K-deep
ring buffer, `trace_push`/`trace_event`, per-register `TRACE_WR_FLAG` /
`TRACE_RD_FLAG` / `BYPASS_RD_FLAG` control, the `OP_*` control-op protocol on
`/dev/motion-trace`) is new code written for this port, modeled on (and
substantially extending) the **idea** from the upstream Pandora-CNC
`motiondev` project for the V1.1 — see
[upstream-research.md](upstream-research.md) §1 for what that ancestor
project's own trace tool looked like. Ours extends the same idea to cover
peripherals the V1.1 ancestor's tool didn't: the handwheel/MPG, keypad, LEDs,
spindle PWM, and the power-fail interrupt, each surfaced as a synthetic
"event" (`EV_BEEP`, `EV_MPG`, `EV_LED`, `EV_SPINDLE`, `EV_KEY`, `EV_BLOCK`,
`EV_OPEN`, `EV_CLOSE`, `EV_POWERFAIL`) alongside raw register reads/writes.
There is nothing in the disassembled vendor binary to compare this against;
it's purely our addition for recovering the FPGA register map (see the
README's "Recommended workflow").

## 5. Locking: semaphore + static buffer vs. the vendor's own

- **Serialization.** Our driver uses one counting semaphore (`bus_sem`,
  `sema_init`/`down`/`up`) around every bus operation. This was a deliberate
  choice, not an accident: an earlier version used a rwsem
  (`down_write`/`up_write`), but the vendor's `__versions` symbol table
  doesn't export the rwsem write ops — only plain `down`/`up` — so keeping a
  semaphore keeps our imported-symbol set a **subset** of the vendor's,
  which is what let us reuse the vendor's real CRCs instead of needing a
  from-scratch kernel build (full story in motiondev-ko.md, "The modversions
  problem"). We don't know what lock, if any, the vendor module itself takes
  around its own bus accessors — the vendor's disassembly notes list its
  extra imports as `disable_irq`, `down_trylock`, `param_get/set_int`, which
  suggests its concurrency model is at least IRQ-aware in a way ours
  currently isn't (we have no `disable_irq` around the bus ops, only the
  module's own semaphore). motiondev-ko.md's open question #2 flags this as
  unresolved: "behaviour differs a little."
- **Trace storage.** The 64K-record ring (`trace_storage`, `TRACE_SIZE *
  sizeof(trace_rec)` = 512 KiB) is a **static `.bss` array**, specifically so
  as not to import `vmalloc`/`vfree` — again for the same vendor-CRC-reuse
  reason, not a performance or correctness choice.
- **Module parameters.** The vendor exports `major_no`/`minor_no` as
  module params (confirmed via `param_get/set_int` imports and the
  `Module params` row in motiondev-ko.md); ours has none and always calls
  `alloc_chrdev_region` for a dynamic major. Functionally harmless for
  `ddcsv4.out` (which only opens `/dev/motion` by path), but it means device
  numbers aren't configurable the way the vendor's are.

## 6. Kernel-symbol imports

Reduced by design to be a subset of the vendor's 35, specifically so the
vendor's real `__versions` CRCs could be reused without a full kernel/BSP
build. The three changes made for this and their vendor-side justification
are catalogued in motiondev-ko.md ("Source reduced to the vendor's symbol
set"): `down`/`up` instead of `down_write`/`up_write`, no `vmalloc`/`vfree`
(static buffer instead), and no `preempt_schedule` (an artifact of an
earlier build against generic headers, expected to disappear when built
against the real tree). Expected import count: 30, confirmed as all present
in the vendor's 35. The vendor's remaining imports we **don't** use
(`disable_irq`, `down_trylock`, `param_get/set_int`) are exactly the ones
behind the two known behavioural gaps in §5.

## 7. Straight reimplementation vs. educated guess — summary table

Cross-referencing the README's "Unknowns / assumptions" section against what
motiondev-ko.md has since confirmed on the real unit:

| Item | Status |
|---|---|
| FPGA bus masks/offsets, GPIO numbers, spindle PWM words, MPG `ROTCTRL` value, ioctl jump table, FPGA register numbers (`0x3B`, `0x41`, `0x50`, `0x66`) | **Confirmed** against the device `.ko` — straight reimplementation, not a guess |
| ioctl `0x4622` block word order (straight-through `0x50..0x60`, not the V1.1's reordered/double-latched form) | Still an assumption — the device binary writes straight-through, matching our port, but this hasn't been confirmed against a live bus capture (`mtrace`) yet |
| `DIGCTL_MICROSECONDS` offset `0xC0` | **Still an assumption** — vendor doesn't use this register at all, so there's nothing in the vendor binary to confirm it against; only the i.MX28 reference manual backs it |
| TM1638 2-of-3 glitch filter timing | Logic ported exactly from the vendor's disassembly; the *timing* (clock rate) is not independently verified on a scope |
| ioctl `0x4625` identity (power-status vs. MPG-count) | **Open question**, not yet resolved even after reading the device binary — the TIMROT base is in the ioctl's literal pool but which specific case uses it wasn't traced per-case |
| IRQ-aware locking around bus ops (vendor's `disable_irq`/`down_trylock`) | **Known gap** — vendor imports suggest it, ours doesn't replicate it |
| `Module.symvers` / CRCs | No longer an assumption — generated from the vendor module and round-trip verified byte-for-byte; what remains is a Linux-host rebuild, not reverse engineering |

## Bottom line

For the nine ioctls `ddcsv4.out` actually calls, this port is a **deliberate,
largely-confirmed reimplementation** of the vendor's `/dev/motion`, not a
best-effort approximation — every hardware constant it depends on except the
trace timestamp offset has been checked against the real `motiondev.ko`
binary. The structural differences (trace engine, semaphore vs. vendor
locking, symbol-subset imports) are either pure additions with no vendor
analogue, or deliberate trade-offs made to solve the CRC/modversions problem
documented in motiondev-ko.md. The two real open items before calling this
"hardware-verified" are: (1) which ioctl case actually reads MPG vs. power
status, and (2) whether the vendor's extra IRQ-related imports mean its bus
locking does something ours doesn't during the power-fail path.
