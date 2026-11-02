/* Typed FPGA layer: everything above talks to the FPGA only through these calls. */
#ifndef FPGA_H
#define FPGA_H
#include "bus.h"

typedef struct {
	uint8_t limit_port_pos, limit_port_neg, home_port, alarm_port;  /* 0 = none, 1..18 = INnn */
	bool dir_invert, pulse_high;
	int8_t out;             /* pulse/dir connector: 0 = the axis' own, 1..4 = X Y Z A, -1 = none */
} fpga_axis_cfg_t;

typedef struct {
	bus_t *bus;
	uint8_t outputs;
	uint16_t last_mpg;
	uint32_t writes;
	uint32_t blocks, block_seq;  /* blocks pushed; w2 of the next block */
	/* axis -> channel (user 2026-10-31: a dead X DIR output, X moved to the A connector).
	 * Everything below takes AXES; only this layer knows channels. An axis without a
	 * channel counts its own steps in virt[] so its position check stays consistent. */
	int8_t chan[NAXES];
	int32_t virt[NAXES];
} fpga_t;

/* Measured on the unit: no axis steps faster than ~40 000 steps/s, whatever the rate word
 * (= 1200 mm/min at 2000 steps/mm - the vendor's own max manual speed on this machine). */
#define FPGA_MAX_SPS 40000

typedef struct {
	bool held;          /* dead-man pressed (a multiplier bit is present) */
	int axis;           /* -1 = OFF (also positions 5/6, which have no wires), 0..3 = X Y Z 4 */
	int mult;           /* 1, 10, 100 (only valid while held) */
	bool estop;         /* pendant E-stop pressed */
	uint16_t raw;       /* register 0x3D as read (I/O screen, API) */
} pendant_t;

void fpga_attach(fpga_t *f, bus_t *b);
/* Axis electrical config + FPGA-internal limit/home/alarm port setup, in the vendor's order.
 * cfg is per AXIS; each channel gets the config of the first axis mapped to it. */
void fpga_init(fpga_t *f, const fpga_axis_cfg_t cfg[NAXES]);
uint16_t fpga_version(fpga_t *f);

/* IN01..IN18 as raw electrical levels: bit n-1 = 1 when INn is open (not pulled to COM-). */
uint32_t fpga_inputs(fpga_t *f);
pendant_t fpga_pendant(fpga_t *f);
int16_t fpga_wheel_delta(fpga_t *f);          /* raw counts since last call (wrap-safe) */

void fpga_set_outputs(fpga_t *f, uint8_t mask);  /* OUT1..3 = bits 0..2 */
void fpga_spindle_pwm(fpga_t *f, int duty);      /* 0..3000 */

uint8_t fpga_moving(fpga_t *f);                  /* bit n = axis n's channel is playing a block */
bool fpga_busy(fpga_t *f);                       /* any channel playing a block */
uint16_t fpga_status(fpga_t *f);                 /* raw 0x01: bits 4+ch and 12+ch while a block plays on ch */
/* Watchdog + position latch: must be written at least every ~50 ms or nothing steps. */
void fpga_heartbeat(fpga_t *f);
void fpga_positions(fpga_t *f, int32_t pos[NAXES]);  /* latched counters (call after a heartbeat) */
void fpga_preset(fpga_t *f, int axis, int32_t steps); /* set an axis' position counter */

/* ---- block mode (continuous motion, docs/block-mode.md; fpga_block.c) ----
 * Per channel the vendor's mode write: on = 0x30+ch before a stream, off = 0x20+ch after. */
void fpga_block_mode(fpga_t *f, bool on);
/* ONE block of dt_s seconds from per-AXIS signed step deltas, all channels together; the FPGA
 * queues it behind the others and plays them back to back. Axes without a channel count in
 * virt[]. false = no channel has steps (nothing sent). Needs the watchdog to play. */
bool fpga_block(fpga_t *f, const int32_t steps[NAXES], double dt_s);
#define FPGA_QUEUE_FULL 255                      /* never push at or above this: a full queue reads 0x100 and the block ioctl then hangs */
int fpga_block_queue(fpga_t *f);                 /* blocks queued, 0x100 = full */
void fpga_block_flush(fpga_t *f);                /* empty the queue: mode off/on - TO BE MEASURED */

#endif
