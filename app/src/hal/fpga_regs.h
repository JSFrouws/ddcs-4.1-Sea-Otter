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
#define FW_ALARM_PORT   0x36  /* TRACE: banked, drive-alarm input port << 8 */
#define FW_OUTPUTS      0x3E  /* CONFIRMED: bit n = OUT(n+1), whole mask each time */
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
#define FR_VERSION      0x41  /* CONFIRMED: FPGA version */

#define FR_STATUS_MOVING(ax) (1u << (8 + (ax)))
#define FR_STATUS_ACTIVE(ax) (1u << (ax))

#define PD_X1      (1u << 0)  /* pendant bits 0..6 active-low, only while the dead-man is held */
#define PD_X10     (1u << 1)
#define PD_X100    (1u << 2)
#define PD_AXIS_X  (1u << 3)
#define PD_AXIS_Y  (1u << 4)
#define PD_AXIS_Z  (1u << 5)
#define PD_AXIS_4  (1u << 6)
#define PD_ESTOP   (1u << 9)  /* active-high, not gated */

#endif
