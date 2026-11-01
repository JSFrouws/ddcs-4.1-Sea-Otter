/* bus_t bound to the in-process FPGA model. */
#include <stdlib.h>
#include "bus.h"
#include "../sim/fpga_sim.h"

static void w(bus_t *b, uint8_t a, uint16_t v) { (void)b; sim_write(a, v); }
static uint16_t r(bus_t *b, uint8_t a) { (void)b; return sim_read(a); }
/* as the driver does it: the 16 words to 0x50..0x5F, then the 0x66 latch pulse */
static void blk(bus_t *b, const uint16_t w16[16])
{
	(void)b;
	for (int i = 0; i < 16; i++) sim_write((uint8_t)(0x50 + i), w16[i]);
	sim_write(0x66, 1);
	sim_write(0x66, 0);
}
static void pwm(bus_t *b, int d) { (void)b; SIM.pwm = d; }
static void beep(bus_t *b, bool on) { (void)b; SIM.beep = on; }
static void cl(bus_t *b) { free(b); }

bus_t *bus_sim_open(void)
{
	bus_t *b = calloc(1, sizeof *b);
	b->write = w; b->read = r; b->block = blk; b->pwm = pwm; b->beep = beep; b->close = cl;
	return b;
}
