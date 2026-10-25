/* Run-time estimate (core/estimate.c) against the formula for a simple move, and against the
 * real planner + executor on the simulated machine: the estimate must never be shorter than
 * the run, and not much longer. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "t.h"
#include "core/machine.h"
#include "core/estimate.h"
#include "sim/fpga_sim.h"

/* load, park at the program's start (work zero, Z at safe height, as the estimate assumes),
 * run it on the simulator; returns the run time in seconds (virtual clock, 10 ms ticks) */
static double run_time(const char *name, const char *text, double *est)
{
	arg_t a = { .path = name, .text = text, .len = strlen(text) };
	act(C_LOAD, &a, SRC_PANEL);
	if (!C.prog.loaded) { printf("  %s: %s\n", name, C.prog.err); return -1; }
	est_compute(&C.prog);
	double p[NAXES];
	const double *o = WD.wcs[WD.active].off;
	for (int k = 0; k < NAXES; k++) p[k] = o[k];
	p[AX_Z] = o[AX_Z] + SET.safe_z;
	motion_move_to(&C.mo, p, 0, true);
	fx_until_idle(120000);
	C.resume_line = -1; C.work_done = -1;
	bool left;
	*est = est_now(&left);
	act(C_START, NULL, SRC_PANEL);
	int ms = 0;
	while ((C.m == M_RUN || C.m == M_HOLD) && ms < 3600000) { fx_run(10); ms += 10; }
	return ms / 1000.0;
}

void test_estimate(void)
{
	SUITE("run-time estimate vs the planner");
	fx_reset();
	SET.spindle_delay = 2;

	/* one straight move from rest to rest: L/v + v/a_eff (the S-curve's average acceleration) */
	{
		const char *t1 = "G21 G90\nG1 X100 F600\nM30\n";
		arg_t a = { .path = "line.nc", .text = t1, .len = strlen(t1) };
		act(C_LOAD, &a, SRC_PANEL);
		bool lf;
		CHECK(est_now(&lf) == -2);                          /* worked out in the background ... */
		for (int i = 0; i < 20 && !C.prog.est_cum; i++) fx_run(10);
		CHECK(C.prog.est_cum != NULL);                      /* ... within a few ticks for a small one */
		double v = 10, A = SET.ax[AX_X].accel, J = SET.ax[AX_X].jerk, ae = A * v / (v + A * A / J);
		/* the program first goes from its start (work zero, safe Z) to X0 Z5: 0 mm, then 100 mm */
		CHECK_NEAR(est_span(&C.prog, 0, C.prog.nlines, 100), 100 / v + v / ae, 0.01);
		CHECK_NEAR(est_span(&C.prog, 0, C.prog.nlines, 50), 2 * (100 / v + v / ae), 0.02);   /* below 100 %: x 1/override */
		double t200 = est_span(&C.prog, 0, C.prog.nlines, 200), t150 = est_span(&C.prog, 0, C.prog.nlines, 150);
		double vc = fmin(20, fmin(SET.ax[AX_X].max_vel / 60, FPGA_MAX_SPS * 0.8 / SET.ax[AX_X].steps_per_mm));   /* 200 % is above the FPGA's sustained rate */
		CHECK_NEAR(t200, 100 / vc + vc / ae, 0.02);                    /* (a_eff stays the planner's, from the programmed F) */
		CHECK(t150 > t200 && t150 < est_span(&C.prog, 0, C.prog.nlines, 100));
		CHECK(C.prog.est_s > est_span(&C.prog, 0, C.prog.nlines, 100));                    /* the margin */
	}

	/* real runs on the simulated machine: never under, at most ~15 % over */
	static const struct { const char *name, *text; } P[] = {
		{ "square.nc", "G21 G90\nG0 X0 Y0\nG1 Z-1 F300\nG1 X40 F900\nG1 Y40\nG1 X0\nG1 Y0\nG0 Z5\nM30\n" },
		{ "arcs.nc", "G21 G90\nM3 S10000\nG0 X10 Y0\nG1 Z-1 F300\nG2 X30 Y20 I0 J20 F800\nG3 X10 Y40 I-20 J0\nG2 X10 Y0 I0 J-20\nG0 Z5\nM5\nM30\n" },
		{ "drill.nc", "G21 G90\nG0 X0 Y0\nG81 X10 Y10 Z-5 R2 F200\nX20\nX30\nY20\nX20\nX10\nG80\nG0 Z5\nG4 P1.5\nM30\n" },
		{ "rapids.nc", "G21 G90\nG0 X100 Y0\nG0 X100 Y80\nG0 X0 Y80\nG0 X0 Y0\nM30\n" },
	};
	double worst_under = 0, worst_over = 0;
	for (size_t i = 0; i < ARRAY_LEN(P); i++) {
		double est, t = run_time(P[i].name, P[i].text, &est);
		double r = est / t;
		printf("  %-10s run %6.2f s  estimate %6.2f s  (%+.1f %%)\n", P[i].name, t, est, (r - 1) * 100);
		CHECK(t > 0 && est >= t);                               /* never shorter than the run */
		CHECK(r < 1.25);                                        /* and not wildly longer */
		if (r < 1 + worst_under) worst_under = r - 1;
		if (r - 1 > worst_over) worst_over = r - 1;
	}

	/* many tiny segments (a CAM surface): planner speed limited by corners and lookahead */
	{
		size_t cap = 400000, n = 0;
		char *big = malloc(cap);
		n += (size_t)snprintf(big + n, cap - n, "G21 G90\nG0 X0 Y0\nG1 Z-0.5 F600\n");
		for (int k = 1; k <= 3000; k++) n += (size_t)snprintf(big + n, cap - n, "G1 X%.3f Y%.3f F1200\n", k * 0.05, 3 * sin(k * 0.02));
		n += (size_t)snprintf(big + n, cap - n, "G0 Z5\nM30\n");
		double est, t = run_time("surface.nc", big, &est);
		printf("  %-10s run %6.2f s  estimate %6.2f s  (%+.1f %%)\n", "surface.nc", t, est, (est / t - 1) * 100);
		CHECK(t > 0 && est >= t && est / t < 1.25);
		free(big);
	}

	/* live: the estimate follows the feed override, and counts down while it runs */
	{
		const char *t2 = "G21 G90\nG1 X100 F600\nG1 Y60\nG1 X0\nG1 Y0\nM30\n";
		arg_t a = { .path = "live.nc", .text = t2, .len = strlen(t2) };
		act(C_LOAD, &a, SRC_PANEL);
		est_compute(&C.prog);
		bool left;
		double all = est_now(&left);
		CHECK(!left && all > 0);
		C.mo.feed_ov = 50; double slow = est_now(&left);
		C.mo.feed_ov = 200; double fast = est_now(&left);
		C.mo.feed_ov = 100;
		CHECK(slow > all * 1.8 && fast < all * 0.75);
		act(C_START, NULL, SRC_PANEL);
		fx_run(8000);
		double rem = est_now(&left);
		CHECK(left && rem < all && rem > 0);
		C.mo.feed_ov = 150; double rem2 = est_now(&left);
		CHECK(rem2 < rem);
		C.mo.feed_ov = 100;
		act(C_STOP, NULL, SRC_PANEL);
		fx_until_idle(20000);
	}

	/* starting far from the program: the way there is in it (idle), and while it runs the
	 * remaining time never drops below what is really left (found on the unit: 18 s shown,
	 * 52 s run, after a long approach move) */
	{
		const char *t3 = "G21 G90\nG0 X0 Y0\nG1 Z-1 F300\nG1 X30 F900\nG0 Z5\nM30\n";
		arg_t a = { .path = "far.nc", .text = t3, .len = strlen(t3) };
		act(C_LOAD, &a, SRC_PANEL);
		est_compute(&C.prog);
		bool left;
		const double *o = WD.wcs[WD.active].off;
		double p[NAXES];
		for (int k = 0; k < NAXES; k++) p[k] = o[k];
		p[AX_Z] = o[AX_Z] + SET.safe_z;
		motion_move_to(&C.mo, p, 0, true); fx_until_idle(60000);
		double near = est_now(&left);
		p[AX_X] += 200; p[AX_Y] += 150;
		motion_move_to(&C.mo, p, 0, true); fx_until_idle(60000);
		double far = est_now(&left);
		CHECK(far > near + 250 / 20.0 * 0.9);                 /* ~250 mm of rapid more (diagonal: up to 20 mm/s) */
		act(C_START, NULL, SRC_PANEL);
		int ms = 0; bool ok = true;
		double seen[400]; int ns = 0;
		while ((C.m == M_RUN || C.m == M_HOLD) && ms < 600000) {
			fx_run(10); ms += 10;
			if (ms % 1000 == 0 && ns < 400) seen[ns++] = est_now(&left);
		}
		for (int i = 0; i < ns; i++) ok &= seen[i] >= ms / 1000.0 - (i + 1);   /* at second i+1 */
		printf("  far start: run %.2f s, estimate %.2f s\n", ms / 1000.0, far);
		CHECK(far >= ms / 1000.0 && ok);
	}

	/* two JOB parts: about twice one */
	{
		bool left;
		const double *o = WD.wcs[WD.active].off;           /* parked at the start: no approach in it */
		double p[NAXES];
		for (int k = 0; k < NAXES; k++) p[k] = o[k];
		p[AX_Z] = o[AX_Z] + SET.safe_z;
		motion_move_to(&C.mo, p, 0, true); fx_until_idle(60000);
		double one = est_now(&left);
		memcpy(WD.wcs[1].off, WD.wcs[0].off, sizeof WD.wcs[1].off);
		WD.wcs[1].off[AX_X] += 20;
		bool j0 = WD.wcs[0].job, j1 = WD.wcs[1].job;
		WD.wcs[0].job = WD.wcs[1].job = true;
		double two = est_now(&left);
		CHECK_NEAR(two, 2 * one - EST_MARGIN_S, 0.05);        /* twice the cutting, the fixed margin once */
		WD.wcs[0].job = j0; WD.wcs[1].job = j1;
	}
	SET.spindle_delay = 0;
}
