/* bus_t bound to the in-process FPGA model. */
#include <stdlib.h>
#include "bus.h"
#include "../sim/fpga_sim.h"

static void w(bus_t *b, uint8_t a, uint16_t v) { (void)b; sim_write(a, v); }
static uint16_t r(bus_t *b, uint8_t a) { (void)b; return sim_read(a); }
static void pwm(bus_t *b, int d) { (void)b; SIM.pwm = d; }
static void beep(bus_t *b, bool on) { (void)b; SIM.beep = on; }
static void cl(bus_t *b) { free(b); }

bus_t *bus_sim_open(void)
{
	bus_t *b = calloc(1, sizeof *b);
	b->write = w; b->read = r; b->pwm = pwm; b->beep = beep; b->close = cl;
	return b;
}
