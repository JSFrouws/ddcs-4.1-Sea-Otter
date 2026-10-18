/* Lookahead planner: corner speeds from the per-axis corner jump, braking in time for what
 * is coming, runs of tiny segments at full speed, S-curve limits, exact end points. */
#include <stdlib.h>
#include <string.h>
#include "t.h"
#include "core/machine.h"
#include "sim/fpga_sim.h"

typedef struct { double vmin_mid, vmax, amax, jmax; int ticks; } run_t;

/* run until idle; record speed, acceleration and jerk while `mid` says we're in the window */
static run_t run_path(bool (*mid)(const double *pos))
{
	run_t r = { 1e9, 0, 0, 0, 0 };
	double a_prev = C.mo.a;
	for (int i = 0; i < 20000; i++) {
		fx_run(10);
		r.ticks++;
		double v = C.mo.v, a = C.mo.a;
		if (mid && C.mo.qn == 0 && C.mo.active && r.vmin_mid > 1e8) r.vmin_mid = v;   /* speed entering the last segment */
		r.vmax = fmax(r.vmax, v);
		r.amax = fmax(r.amax, fabs(a));
		static double v_prev;
		/* jerk while moving; at a standstill (end of a move, turning point of a reversal) the
		 * leftover deceleration is dropped, which moves nothing */
		if (v > 0.5 && v_prev > 0.5) r.jmax = fmax(r.jmax, fabs(a - a_prev) / 0.010);
		v_prev = v;
		a_prev = a;
		if (motion_idle(&C.mo) && !SIM.left[0] && !SIM.left[1]) break;
	}
	return r;
}

static double X0, Y0;
static bool mid_corner(const double *p) { return p[0] > X0 + 25 && p[0] < X0 + 35; }

static run_t corner(double deg, double feed)
{
	double t[NAXES];
	memcpy(t, C.mo.pos, sizeof t);
	X0 = t[0]; Y0 = t[1];
	t[0] += 30; motion_move_to(&C.mo, t, feed, false);
	t[0] += 30 * cos(deg * M_PI / 180); t[1] += 30 * sin(deg * M_PI / 180);
	motion_move_to(&C.mo, t, feed, false);
	run_t r = run_path(mid_corner);
	CHECK_NEAR(C.mo.pos[0], t[0], 1e-9); CHECK_NEAR(C.mo.pos[1], t[1], 1e-9);
	return r;
}

void test_planner(void)
{
	SUITE("lookahead planner");
	fx_reset();
	double F = 1200, vF = F / 60;                          /* 20 mm/s */

	/* a 10 degree bend: Y starts from 0 but only needs v*sin(10) - no stop at the corner */
	run_t r = corner(10, F);
	printf("  10 deg corner: min speed %.1f of %.1f mm/s\n", r.vmin_mid, vF);
	CHECK(r.vmin_mid > 0.8 * vF);

	/* 90 degrees: X must drop to ~its corner jump (300 mm/min = 5 mm/s) */
	r = corner(90, F);
	printf("  90 deg corner: min speed %.1f mm/s\n", r.vmin_mid);
	CHECK(r.vmin_mid < 5.5 && r.vmin_mid > 1.0);

	/* reversal: X needs 2v -> slows to about half the corner jump, not to a dead stop */
	r = corner(180, F);
	printf("  reversal: min speed %.1f mm/s\n", r.vmin_mid);
	CHECK(r.vmin_mid < 3.0);

	/* acceleration and jerk never exceed the per-axis limits (300 mm/s², 3000 mm/s³) */
	printf("  peak accel %.0f mm/s², peak jerk %.0f mm/s³\n", r.amax, r.jmax);
	CHECK(r.amax <= SET.ax[0].accel * 1.001);
	CHECK(r.jmax <= SET.ax[0].jerk * 1.05);
	CHECK(r.vmax <= vF * 1.0001);

	/* 3000 tiny collinear segments (0.01 mm, engraving-style): the planner looks far enough
	 * ahead by distance, so the feed is reached instead of crawling segment by segment */
	double t[NAXES];
	memcpy(t, C.mo.pos, sizeof t);
	double x0 = t[0];
	int pushed = 0;
	double vpeak = 0;
	while (pushed < 3000 || !motion_idle(&C.mo)) {
		while (pushed < 3000 && motion_wants_more(&C.mo)) { t[0] += 0.01; motion_move_to(&C.mo, t, F, false); pushed++; }
		fx_run(10);
		vpeak = fmax(vpeak, C.mo.v);
		if (motion_idle(&C.mo) && pushed >= 3000) break;
	}
	fx_run(50);
	printf("  3000 x 0.01 mm segments: peak %.1f of %.1f mm/s\n", vpeak, vF);
	double vcap = FPGA_MAX_SPS * 0.8 / SET.ax[0].steps_per_mm;   /* sustained-burst cap: 16 mm/s */
	CHECK(vpeak > 0.95 * vcap);
	CHECK_NEAR(C.mo.pos[0], x0 + 30, 1e-6);

	/* braking ahead: a long move into a reversal brakes in time, within the acceleration limit */
	memcpy(t, C.mo.pos, sizeof t);
	double xr = t[0] + 40;
	t[0] = xr; motion_move_to(&C.mo, t, F, false);
	t[0] = xr - 10; motion_move_to(&C.mo, t, F, false);
	double amax = 0;
	for (int i = 0; i < 2000; i++) {
		fx_run(10);
		amax = fmax(amax, fabs(C.mo.a));
		if (motion_idle(&C.mo)) break;
	}
	CHECK(amax <= SET.ax[0].accel * 1.001);
	CHECK_NEAR(C.mo.pos[0], xr - 10, 1e-9);

	/* feed override lowered mid-move: speed follows down smoothly, applied % reported */
	memcpy(t, C.mo.pos, sizeof t);
	t[1] += 60; motion_move_to(&C.mo, t, F, false);
	fx_run(1500);
	CHECK_NEAR(C.mo.applied_pct, 100, 1);
	C.mo.feed_ov = 50;
	fx_run(1500);
	CHECK_NEAR(C.mo.applied_pct, 50, 1);
	C.mo.feed_ov = 100;
	run_path(NULL);
	CHECK_NEAR(C.mo.pos[1], t[1], 1e-9);
	CHECK(SIM.ignored_cmds == 0);

	/* with the FPGA's 2 ms burst overhead modelled, a full-speed move keeps the backlog of
	 * planned-but-unsent steps under one burst, and stops overshoot little */
	bool gs = SET.graceful_stop;
	for (int mode = 0; mode < 2; mode++) {
		SET.graceful_stop = mode == 1;
		memcpy(t, C.mo.pos, sizeof t);
		t[0] += 60; motion_move_to(&C.mo, t, 3000, false);    /* asks more than the cap: capped */
		int64_t backlog = 0;
		for (int k = 0; k < 150; k++) {
			fx_run(10);
			int64_t b = llabs(llround(C.mo.pos[0] * SET.ax[0].steps_per_mm) - C.mo.steps[0]);
			if (k > 50 && b > backlog) backlog = b;
		}
		double v_at = C.mo.v, at = sim_pos_mm(0);
		motion_stop(&C.mo, mode == 0);
		for (int k = 0; k < 300 && (!motion_idle(&C.mo) || SIM.left[0]); k++) fx_run(10);
		fx_run(50);
		double over = sim_pos_mm(0) - at;
		printf("  %s stop at %.1f mm/s: overshoot %.2f mm, backlog %lld steps\n", mode ? "graceful" : "abrupt", v_at, over, (long long)backlog);
		CHECK(backlog <= (int64_t)(FPGA_MAX_SPS * 0.030) + 1);
		CHECK(v_at <= FPGA_MAX_SPS * 0.8 / SET.ax[0].steps_per_mm + 1e-6);
		CHECK(over < (mode ? 1.5 : 0.8));
		CHECK_NEAR(sim_pos_mm(0), C.mo.pos[0], 0.002);
	}
	SET.graceful_stop = gs;

	/* 300 % feed override never plans above the cap */
	memcpy(t, C.mo.pos, sizeof t);
	t[1] += 30; C.mo.feed_ov = 300;
	motion_move_to(&C.mo, t, 1000, false);
	run_t r3 = run_path(NULL);
	CHECK(r3.vmax <= FPGA_MAX_SPS * 0.8 / SET.ax[1].steps_per_mm + 1e-6);
	C.mo.feed_ov = 100;
}
