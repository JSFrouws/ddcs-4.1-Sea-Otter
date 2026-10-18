#include <stdio.h>
#include <string.h>
#include "t.h"
#include "core/machine.h"
#include "sim/fpga_sim.h"

int t_fail, t_pass;

void test_fpga(void);
void test_gcode(void);
void test_motion(void);
void test_machine(void);
void test_ui(void);
void test_planner(void);

static bus_t *bus;

void fx_reset(void)
{
	clock_use_virtual(true);
	sim_reset_machine();
	if (!bus) bus = bus_sim_open();
	bus->trace = NULL;
	remove("t_settings.cfg"); remove("t_work.cfg"); remove("t_work.cfg.pos");
	program_free(&C.prog);
	ctl_init(bus, true, "t_settings.cfg", "t_work.cfg");
}

void fx_run(int ms)
{
	for (int t = 0; t < ms; t += TICK_MS) {
		ctl_tick(TICK_MS / 1000.0);
		sim_step(TICK_MS);
		clock_advance(TICK_MS);
	}
}

void fx_until_idle(int max_ms)
{
	for (int t = 0; t < max_ms; t += TICK_MS) {
		fx_run(TICK_MS);
		if (motion_idle(&C.mo) && (C.m == M_IDLE || C.m == M_LIMIT) && !SIM.left[0] && !SIM.left[1] && !SIM.left[2] && !SIM.left[3]) {
			fx_run(20);
			return;
		}
	}
}

int main(void)
{
	test_fpga();
	test_gcode();
	test_motion();
	test_planner();
	test_machine();
	test_ui();
	printf("\n%d passed, %d failed\n", t_pass, t_fail);
	return t_fail ? 1 : 0;
}
