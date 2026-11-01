/* FPGA register map as far as it is known (docs/io-map.md, docs/fpga-outputs.md,
 * docs/mpg-pendant-io.md, and the vendor app trace docs/raw/2026-10-16_fpga-outputs).
 * Read and write registers at the same address are different things.
 * CONFIRMED = proven on hardware; TRACE = copied from the vendor app's own writes,
 * meaning inferred; ASSUMPTION = our guess, flagged where used. */
#ifndef FPGA_REGS_H
#define FPGA_REGS_H

/* ---- write registers ---- */
#define FW_AXIS_SEL     0x00  /* CONFIRMED: banked-register axis select 0..3 = X Y Z A, 0x0F = none */
#define FW_AXIS_ENABLE  0x01  /* TRACE: banked, per-axis I/O function enables (see fpga_init) */
#define FW_AXIS_CFG     0x02  /* CONFIRMED: banked, bit1 DIR invert, bit2 pulse idle-high */
#define FW_MOVE_MODE    0x03  /* TRACE: always 2 for jog bursts */
#define FW_MOVE_CMD     0x04  /* CONFIRMED: bit0 go, bit1 DIR; written back to 0 after.
                               * 4 = STOP (CONFIRMED 2026-10-18: decelerates within ~20 ms), but leaves
                               * the axis in a held state that ignores new bursts - release unknown */
#define FW_POS_PRESET   0x05  /* CONFIRMED 2026-10-18 (vendor init): banked, 1 -> 0 loads 0x0C/0x0D into the position counter */
#define FW_SOFT_MAX_LO  0x06  /* CONFIRMED: banked FPGA soft limit +, 32 bit steps (06/07); - in 08/09 */
#define FW_PRESET_LO    0x0C  /* CONFIRMED: banked 32-bit value for the position preset (0C/0D) */
#define FW_STEPS_LO     0x0A  /* CONFIRMED: banked burst step count, low 16 */
#define FW_STEPS_HI     0x0B  /*            high 16 */
#define FW_RATE_A_LO    0x12  /* TRACE: 0x208D with every jog burst (rate/period word) */
#define FW_RATE_A_HI    0x13
#define FW_RATE_B_LO    0x14  /* TRACE: 0x2710 with every jog burst */
#define FW_RATE_B_HI    0x15
#define FW_STATUS_ACK   0x2E  /* TRACE: 0 written before each status sweep (not the done-ack, tested) */
#define FW_LIMIT_PORTS  0x34  /* TRACE: banked, low byte = +limit input port, high = -limit */
#define FW_HOME_PORT    0x35  /* TRACE: banked, home input port */
#define FW_ALARM_PORT   0x36  /* TRACE: banked, drive-alarm input port << 8 (READ 0x36 = FR_BLOCK_QUEUE, unrelated) */
#define FW_OUTPUTS      0x3E  /* CONFIRMED: bit n = OUT(n+1), whole mask each time */
#define FW_AXIS_MODE    0x42  /* CONFIRMED 2026-11-01 (vendor trace, copied in blockmove): banked by 0x00;
                               * 0x30+ch before a block stream = block mode, 0x20+ch after it */
#define FW_BLOCK_FIRST  0x50  /* CONFIRMED 2026-11-01: 16 block words 0x50..0x5F (see BLOCK WORDS below) */
#define FW_BLOCK_LATCH  0x66  /* CONFIRMED 2026-11-01: 1 then 0 pushes the staged block into the queue
                               * (the vendor driver's ioctl 0x4622 does words + latch itself) */
#define FW_HEARTBEAT    0x67  /* CONFIRMED 2026-10-18: motion WATCHDOG + position latch. Without it a burst
                               * is accepted but never stepped; each write latches the position counters */

/* ---- read registers ---- */
#define FR_SELECTED     0x00  /* CONFIRMED: after a finished move shows 0x80 | axis bit (done latch) */
#define FR_STATUS       0x01  /* CONFIRMED 2026-10-18: per axis n: bit n = move active (stays set after a
                               * STOP = held), bit 8+n = stepping (the old "bit 9 busy" was axis Y) */
#define FR_POS_LO       0x02  /* CONFIRMED 2026-10-18: position counters, 32-bit steps, X 02/03 (lo/hi),
                               * Y 04/05, Z 06/07, A 08/09; latched by each 0x67 write; keep their value
                               * while the unit is powered (the vendor reads them at start-up) */
#define FR_MPG_COUNT    0x0E  /* CONFIRMED: 16-bit wrapping handwheel count (4 per detent) */
#define FR_ALARM        0x1D  /* CONFIRMED: bit0 IN01 (E-stop input fn), bit1 pendant E-stop latch */
#define FR_AXIS_IOSTAT  0x1E  /* CONFIRMED: 0x1E..0x21 per-axis limit/home (0x30) / alarm (0x100) */
#define FR_INPUTS_LO    0x37  /* CONFIRMED: IN01..IN16 = bits 0..15, active-low (closed to COM-) */
#define FR_INPUTS_HI    0x38  /* CONFIRMED: IN17 bit0, IN18 bit1 */
#define FR_KEYS_EXT     0x3B  /* CONFIRMED: IN15..18 mirrored, active-high */
#define FR_PENDANT      0x3D  /* CONFIRMED: see PD_* */
#define FR_BLOCK_QUEUE  0x36  /* CONFIRMED 2026-11-01: blocks queued, 0xff = full (256 deep). Not the
                               * register written at 0x36 (FW_ALARM_PORT, banked): read and write differ */
#define FR_VERSION      0x41  /* CONFIRMED: FPGA version */

#define FR_STATUS_MOVING(ax) (1u << (8 + (ax)))
#define FR_STATUS_ACTIVE(ax) (1u << (ax))
/* block mode uses other bits: 0x2020 while a stream plays on channel 1 (measured 2026-11-01) */
#define FR_STATUS_BLOCK(ch)  (1u << (4 + (ch)) | 1u << (12 + (ch)))

/* BLOCK WORDS (decoded + driven on the unit 2026-11-01, docs/fpga-motion.md "Block mode"):
 *   w0   flags: bit 9 = direction of the dominant channel (1 = +), bits 8/7/6 = directions of
 *        the remaining channels in index order, bits 1-2 = dominant channel (X 0, Y 2, Z 4,
 *        A 6), bit 0 = feed (1) / rapid (0, vendor G0), bit 5 always set.
 *        vendor: X+ 03e1, X- 01e1, Y+ 03e3, Y- 01e3, X-Y- 00e1, Z 03e4
 *   w1   0x3020, constant
 *   w2   sequence id (the vendor: per G-code line; any value works)
 *   w4   rate word of the dominant channel = steps/s / 2 (as the burst rate 0x12)
 *   w6   0x0d05 (3333); 0x126a on the vendor's 45 deg diagonal - NOT a time base (3333 and
 *        4714 give the same 1.385 s), kept at 3333
 *   w8   steps of the dominant channel in this block (20 mm = 410 blocks, sum 8000 exact)
 *   w10, w12, w14  steps of the remaining channels in index order (Y dominant: X, Z, A;
 *        Z rode in w12)
 *   others 0. A block lasts w8 / (2 * w4) s; the queue plays back to back with no gap,
 *   direction may change from one block to the next, an empty queue stops the axes. */
#define BLK_W0_BASE     0x0021u  /* bit 5 + feed */
#define BLK_W0_DOM_DIR  0x0200u
#define BLK_W1          0x3020u
#define BLK_W6          0x0d05u

#define PD_X1      (1u << 0)  /* pendant bits 0..6 active-low, only while the dead-man is held */
#define PD_X10     (1u << 1)
#define PD_X100    (1u << 2)
#define PD_AXIS_X  (1u << 3)
#define PD_AXIS_Y  (1u << 4)
#define PD_AXIS_Z  (1u << 5)
#define PD_AXIS_4  (1u << 6)
#define PD_ESTOP   (1u << 9)  /* active-high, not gated */

#endif
