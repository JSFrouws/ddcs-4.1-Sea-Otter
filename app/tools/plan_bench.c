/* plan_bench: run a G-code file through the controller + FPGA simulator in virtual time and
 * report how long it takes, for a few planner settings. Host only.
 *   gcc ... -Isrc -o build/plan_bench tools/plan_bench.c $(CORE) -lm
 *   build/plan_bench programs/Sign_rosette.nc  "jerk corner_jump accel" ...  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/machine.h"
#include "sim/fpga_sim.h"

static double run(const char *path, double jerk, double jump, double accel)
{
	static bus_t *bus;
	clock_use_virtual(true);
	sim_reset_machine();
	if (!bus) bus = bus_sim_open();
	remove("pb.cfg"); remove("pb.wd"); remove("pb.wd.pos");
	program_free(&C.prog);
	ctl_init(bus, true, "pb.cfg", "pb.wd");
	SET.spindle_delay = 0;
	for (int a = 0; a < NAXES; a++) { SET.ax[a].jerk = jerk; SET.ax[a].corner_jump = jump; SET.ax[a].accel = accel; }
	for (int a = 0; a < 3; a++) WD.wcs[0].off[a] = C.mo.pos[a] - (a == 2 ? 10 : 50);
	arg_t l = { .path = path };
	if (!act(C_LOAD, &l, SRC_PANEL) || !C.prog.loaded) { printf("load failed: %s\n", C.prog.err); exit(1); }
	act(C_START, NULL, SRC_PANEL);
	uint64_t t = 0;
	while (t < 3600000) {
		ctl_tick(TICK_MS / 1000.0); sim_step(TICK_MS); clock_advance(TICK_MS); t += TICK_MS;
		if (C.m == M_HOLD) act(C_START, NULL, SRC_PANEL);
		if (C.m == M_IDLE && motion_idle(&C.mo)) break;
	}
	return t / 1000.0;
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s file.nc [\"jerk jump accel\"...]\n", argv[0]); return 2; }
	const char *def[] = { "3000 300 300", "10000 300 300", "30000 300 300", "1000000 300 300", "10000 600 300", "1000000 0 300" };
	int n = argc > 2 ? argc - 2 : (int)(sizeof def / sizeof *def);
	for (int i = 0; i < n; i++) {
		double j, c, a;
		sscanf(argc > 2 ? argv[i + 2] : def[i], "%lf %lf %lf", &j, &c, &a);
		printf("jerk %8.0f mm/s3  corner jump %4.0f mm/min  accel %4.0f mm/s2  ->  %6.1f s\n", j, c, a, run(argv[1], j, c, a));
		fflush(stdout);
	}
	return 0;
}
