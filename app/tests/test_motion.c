/* Motion layer driving the simulated FPGA: positions match to the step, speed and
 * acceleration limits hold, hold/resume works, the rapid-follows-feed rule. */
#include <string.h>
#include "t.h"
#include "core/machine.h"
#include "sim/fpga_sim.h"

static double phys(int a) { return sim_pos_mm(a); }

void test_motion(void)
{
	SUITE("motion vs simulated FPGA");
	fx_reset();
	motion_t *m = &C.mo;

	/* the commanded position and the simulated motor agree before anything moves */
	for (int a = 0; a < 3; a++) CHECK_NEAR(phys(a), m->pos[a], 1e-3);

	/* a diagonal feed move lands exactly, and never exceeds the feed */
	double t[NAXES]; memcpy(t, m->pos, sizeof t);
	t[0] += 20; t[1] -= 10;
	motion_move_to(m, t, 1200, false);
	double maxv = 0, prev[2] = { phys(0), phys(1) };
	for (int i = 0; i < 400 && !motion_idle(m); i++) {
		fx_run(10);
		double dx = phys(0) - prev[0], dy = phys(1) - prev[1];
		(void)dx; (void)dy;
		double v = m->v * 60;                 /* commanded path speed */
		if (v > maxv) maxv = v;
		prev[0] = phys(0); prev[1] = phys(1);
	}
	fx_run(30);
	CHECK(motion_idle(m));
	CHECK_NEAR(phys(0), t[0], 0.0005); CHECK_NEAR(phys(1), t[1], 0.0005);
	CHECK(m->steps[0] == llround(t[0] * 2000));
	/* feed, or the per-axis FPGA cap along this direction (X carries 20/sqrt(500) of the path) */
	double cap = fmin(1200, CAP_SPS / 2000 * 60 / (20 / sqrt(500.0)));
	CHECK(maxv <= cap * 1.02);
	CHECK(maxv >= cap * 0.95);

	/* rapid follows feed override down, but never above 100 % */
	CHECK(rapid_override(40) == 40);
	CHECK(rapid_override(100) == 100);
	CHECK(rapid_override(150) == 100);

	/* hold mid-move decelerates to a stop, resume finishes the move */
	memcpy(t, m->pos, sizeof t);
	t[2] -= 30;
	motion_move_to(m, t, 3000, true);
	fx_run(300);
	motion_hold(m, true);
	fx_run(500);
	double zh = phys(2);
	fx_run(300);
	CHECK_NEAR(phys(2), zh, 1e-9);              /* stopped */
	CHECK(phys(2) > t[2] + 1);                  /* not finished */
	motion_hold(m, false);
	fx_run(3000);
	CHECK_NEAR(phys(2), t[2], 0.0005);

	/* feed override scales speed; 0 % stops */
	memcpy(t, m->pos, sizeof t);
	t[0] -= 40;
	m->feed_ov = 50;
	motion_move_to(m, t, 1200, false);
	fx_run(1000);
	double x1 = phys(0);
	fx_run(100);
	CHECK_NEAR((x1 - phys(0)) / 0.1 * 60, 0.5 * fmin(1200, CAP_SPS / 2000 * 60), 15);
	m->feed_ov = 100;
	fx_until_idle(5000);
	CHECK_NEAR(phys(0), t[0], 0.0005);

	/* pendant nudges: 0.001 mm counts add up exactly */
	double x0 = m->pos[0];
	for (int i = 0; i < 37; i++) motion_nudge(m, 0, 0.001);
	fx_run(100);
	double at100 = phys(0);
	fx_run(900);                                       /* pendant.smooth_ms 200 (2026-11-01): the queue
	                                                    * settles exponentially, floor 0.05 mm/s; the
	                                                    * 37 clicks in one slice read as a fast wheel
	                                                    * (feedforward), so most of it is done at once */
	printf("  37 x 0.001 mm nudges: %.4f of 0.037 mm after 100 ms, %.4f after 1 s\n", at100 - x0, phys(0) - x0);
	CHECK(at100 - x0 > 0.005 && at100 - x0 < 0.0365);  /* under way, the last bit settling */
	CHECK_NEAR(phys(0), x0 + 0.037, 0.00051);

	/* a long rapid: blocks respect the FPGA ceiling measured on the unit (40 000 steps/s) */
	uint32_t b0 = C.fp.blocks;
	memcpy(t, m->pos, sizeof t);
	t[1] += 100;
	motion_move_to(m, t, 0, true);
	fx_until_idle(10000);
	CHECK(C.fp.blocks > b0);
	CHECK_NEAR(phys(1), t[1], 0.0005);
	CHECK(SIM.crashes == 0);

	/* zig-zag: every corner reverses X, from one block to the next - and not one step is lost */
	memcpy(t, m->pos, sizeof t);
	for (int i = 0; i < 12; i++) { t[0] += (i & 1) ? -3 : 3; t[1] += 0.5; motion_move_to(m, t, 2400, false); }
	fx_until_idle(30000);
	CHECK_NEAR(phys(0), t[0], 0.0005); CHECK_NEAR(phys(1), t[1], 0.0005);
	CHECK(SIM.ignored_cmds == 0);

	/* homing (2026-11-02): every phase ramps at the axis' acceleration - the search onto the
	 * switch, the slow back-off, the retract - and the retract lands exactly on
	 * home_pos - dir * back-off with the switch released. X homes negative onto a switch
	 * at 0, from 30 mm away. */
	fx_reset();
	m = &C.mo;
	if (C.m == M_OFF) act(C_DRIVES_ON, NULL, SRC_PANEL);
	CHECK(C.m == M_IDLE && phys(0) > 20);
	double A = SET.ax[AX_X].accel, vmax_seen = 0, dvmax = 0, vprev = 0, xprev = phys(0);
	CHECK(act(C_HOME, NULL, SRC_PANEL) && C.m == M_HOMING);   /* through the controller: a switch is no limit while homing */
	int ticks = 0;
	for (; ticks < 20000 && m->home.on; ticks++) {
		fx_run(TICK_MS);
		double v = (phys(0) - xprev) / (TICK_MS / 1000.0);
		if (fabs(v) > vmax_seen) vmax_seen = fabs(v);
		if (fabs(v - vprev) > dvmax) dvmax = fabs(v - vprev);
		vprev = v; xprev = phys(0);
	}
	fx_run(50);
	printf("  homing X: %d ticks, top %.1f mm/s (search %.1f), max dv/tick %.2f (A dt = %.2f), ends at %.4f\n",
		ticks, vmax_seen, SET.ax[AX_X].home_search / 60, dvmax, A * TICK_MS / 1000.0, phys(0));
	CHECK(C.homed && C.m == M_IDLE);
	CHECK(vmax_seen <= SET.ax[AX_X].home_search / 60 * 1.02);
	CHECK(dvmax <= A * TICK_MS / 1000.0 * 1.05 + 0.01);                /* no jump: ramped at <= A */
	CHECK(!sim_switch_active(AX_X));
	CHECK_NEAR(m->pos[0], SET.ax[AX_X].home_pos + SET.ax[AX_X].home_backoff, 0.0005);   /* dir -: retract is + */
	CHECK_NEAR(phys(0), m->pos[0], 0.015);              /* the switch edge is found within one tick at the locate speed */

	/* separate home inputs (2026-11-02): X homes on IN07 (a second input on the same switch
	 * in the sim), the limit stays on IN02; off, homing reads the limit input */
	CHECK(axis_home_port(AX_X) == SET.ax[AX_X].limit_port);
	SET.home_combined = false; SET.ax[AX_X].home_port = 7; SIM.home_input[AX_X] = 7;
	CHECK(axis_home_port(AX_X) == 7);
	SET.ax[AX_X].limit_port = 0;                        /* no limit input at all: homing must still find IN07 */
	memcpy(t, m->pos, sizeof t); t[0] += 20; motion_move_to(m, t, 0, true); fx_until_idle(10000);
	CHECK(act(C_HOME, NULL, SRC_PANEL) && C.m == M_HOMING);
	fx_until_idle(120000);
	CHECK(C.homed && C.m == M_IDLE);
	CHECK_NEAR(m->pos[0], SET.ax[AX_X].home_pos + SET.ax[AX_X].home_backoff, 0.0005);
	SET.ax[AX_X].limit_port = 2; SET.home_combined = true; SIM.home_input[AX_X] = 0;

	/* one axis from the Home menu (2026-11-02): only Y moves, homed stays as it was */
	CHECK(C.homed);
	double xk = phys(0), zk = phys(2);
	memcpy(t, m->pos, sizeof t); t[1] += 15; motion_move_to(m, t, 0, true); fx_until_idle(10000);
	arg_t hy = { .kind = 1, .axis = AX_Y };
	CHECK(act(C_HOME, &hy, SRC_PANEL) && C.m == M_HOMING);
	fx_until_idle(120000);
	CHECK(C.homed && C.m == M_IDLE);
	CHECK_NEAR(m->pos[1], SET.ax[AX_Y].home_pos + SET.ax[AX_Y].home_backoff, 0.0005);
	CHECK_NEAR(phys(0), xk, 1e-9); CHECK_NEAR(phys(2), zk, 1e-9);

	/* switch level per axis: Y's level flipped (NO) while the sim wires NC = Y reads tripped */
	CHECK(axis_level_high(AX_Y) == SET.limit_level_high);
	SET.limit_level_per_axis = true;
	CHECK(axis_level_high(AX_Y) == SET.limit_level_high);   /* turning it on changes nothing */
	SET.ax[AX_Y].limit_level_high = !SET.limit_level_high;
	fx_run(50);
	CHECK(C.lim_active[AX_Y] && C.m == M_LIMIT);
	SET.ax[AX_Y].limit_level_high = SET.limit_level_high; SET.limit_level_per_axis = false;
	fx_run(50);
	CHECK(!C.lim_active[AX_Y]);
}
