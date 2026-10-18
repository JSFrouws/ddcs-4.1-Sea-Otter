/* Typed FPGA layer: everything above talks to the FPGA only through these calls. */
#ifndef FPGA_H
#define FPGA_H
#include "bus.h"

typedef struct {
	uint8_t limit_port_pos, limit_port_neg, home_port, alarm_port;  /* 0 = none, 1..18 = INnn */
	bool dir_invert, pulse_high;
} fpga_axis_cfg_t;

typedef struct {
	bus_t *bus;
	uint8_t outputs;
	uint16_t last_mpg;
	uint32_t bursts, writes;
} fpga_t;

/* Measured on the unit: no axis steps faster than ~40 000 steps/s, whatever the rate word
 * (= 1200 mm/min at 2000 steps/mm - the vendor's own max manual speed on this machine). */
#define FPGA_MAX_SPS 40000

typedef struct {
	bool held;          /* dead-man pressed (a multiplier bit is present) */
	int axis;           /* -1 = OFF (also positions 5/6, which have no wires), 0..3 = X Y Z 4 */
	int mult;           /* 1, 10, 100 (only valid while held) */
	bool estop;         /* pendant E-stop pressed */
} pendant_t;

void fpga_attach(fpga_t *f, bus_t *b);
/* Axis electrical config + FPGA-internal limit/home/alarm port setup, in the vendor's order. */
void fpga_init(fpga_t *f, const fpga_axis_cfg_t cfg[NAXES]);
uint16_t fpga_version(fpga_t *f);

/* IN01..IN18 as raw electrical levels: bit n-1 = 1 when INn is open (not pulled to COM-). */
uint32_t fpga_inputs(fpga_t *f);
pendant_t fpga_pendant(fpga_t *f);
int16_t fpga_wheel_delta(fpga_t *f);          /* raw counts since last call (wrap-safe) */

void fpga_set_outputs(fpga_t *f, uint8_t mask);  /* OUT1..3 = bits 0..2 */
void fpga_spindle_pwm(fpga_t *f, int duty);      /* 0..3000 */

/* One step burst on one axis, the vendor's MPG-jog sequence. steps > 0 = DIR bit set = the
 * position counter counts up (confirmed on the unit). rate = top step rate in steps/s
 * (measured: max rate = 2 x rate word A); 0 = the vendor's jog words (about 16 700 steps/s).
 * A burst sent while the axis still steps in the SAME direction is appended; one in the
 * OPPOSITE direction is silently IGNORED by the FPGA - callers must wait (fpga_moving). */
void fpga_burst(fpga_t *f, int axis, int32_t steps, uint32_t rate);
uint8_t fpga_moving(fpga_t *f);                  /* bit n = axis n active or stepping */
/* The vendor's post-burst status sweep (0x2E = 0, then select each axis). */
void fpga_status_sweep(fpga_t *f);
bool fpga_busy(fpga_t *f);                       /* any axis stepping */
uint16_t fpga_status(fpga_t *f);                 /* raw 0x01: bit n active, bit 8+n stepping */
/* Watchdog + position latch: must be written at least every ~50 ms or nothing steps. */
void fpga_heartbeat(fpga_t *f);
void fpga_positions(fpga_t *f, int32_t pos[NAXES]);  /* latched counters (call after a heartbeat) */
void fpga_preset(fpga_t *f, int axis, int32_t steps); /* set an axis' position counter */
void fpga_stop(fpga_t *f, int axis);             /* 0x04 = 4: decelerating stop (leaves the axis held!) */

#endif
