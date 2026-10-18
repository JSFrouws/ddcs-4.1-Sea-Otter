/* Register bus to the FPGA - the lowest layer.
 *
 * Two implementations with the same interface:
 *   bus_ioctl.c  the vendor's own motiondev.ko (/dev/motion, loaded by rcS at boot):
 *                MOTION_IOC_WRITE 0x4620 / READ 0x4621 / SPINDLE 0x4627 / BEEP 0x4623-24.
 *                Reused as-is: it is CRC-valid for this kernel and proven (docs/fpga-outputs.md),
 *                while our own rebuilt driver can't load without the BSP's Module.symvers.
 *   bus_sim.c    an in-process FPGA model (sim/fpga_sim.c) at register level, so every
 *                layer above runs unchanged against either.
 * An optional trace hook records every access - used by the tests to compare our
 * register sequences with the vendor app's recorded ones (docs/raw/...iotrace_run3.log).
 */
#ifndef BUS_H
#define BUS_H
#include "../common.h"

typedef struct bus bus_t;
struct bus {
	void (*write)(bus_t *, uint8_t addr, uint16_t val);
	uint16_t (*read)(bus_t *, uint8_t addr);
	void (*pwm)(bus_t *, int duty);            /* spindle PWM6, 0..3000 = 0..10 V on VSO */
	void (*beep)(bus_t *, bool on);
	void (*close)(bus_t *);
	void *priv;
	/* trace hook: dir 'W' / 'R' / 'P' */
	void (*trace)(void *ctx, char dir, uint8_t addr, uint16_t val);
	void *trace_ctx;
};

bus_t *bus_ioctl_open(const char *dev);      /* NULL on failure (errno set) */
bus_t *bus_sim_open(void);                   /* sim lives in sim/fpga_sim.h */

static inline void bus_w(bus_t *b, uint8_t a, uint16_t v)
{
	if (b->trace) b->trace(b->trace_ctx, 'W', a, v);
	b->write(b, a, v);
}
static inline uint16_t bus_r(bus_t *b, uint8_t a)
{
	uint16_t v = b->read(b, a);
	if (b->trace) b->trace(b->trace_ctx, 'R', a, v);
	return v;
}
static inline void bus_pwm(bus_t *b, int duty)
{
	if (b->trace) b->trace(b->trace_ctx, 'P', 0, (uint16_t)duty);
	b->pwm(b, duty);
}

#endif
