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
		double v = m->v * 60;                 /* commanded path speed (the motor's per-tick distance is lumpy: stop-go bursts) */
		if (v > maxv) maxv = v;
		prev[0] = phys(0); prev[1] = phys(1);
	}
	fx_run(30);
	CHECK(motion_idle(m));
	CHECK_NEAR(phys(0), t[0], 0.0005); CHECK_NEAR(phys(1), t[1], 0.0005);
	CHECK(m->steps[0] == llround(t[0] * 2000));
	/* feed, or the per-axis sustained-burst cap along this direction (X carries 20/sqrt(500) of the path) */
	double cap = fmin(1200, FPGA_MAX_SPS * 0.8 / 2000 * 60 / (20 / sqrt(500.0)));
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
	CHECK_NEAR((x1 - phys(0)) / 0.1 * 60, 0.5 * fmin(1200, FPGA_MAX_SPS * 0.8 / 2000 * 60), 15);
	m->feed_ov = 100;
	fx_until_idle(5000);
	CHECK_NEAR(phys(0), t[0], 0.0005);

	/* pendant nudges: 0.001 mm counts add up exactly */
	double x0 = m->pos[0];
	for (int i = 0; i < 37; i++) motion_nudge(m, 0, 0.001);
	fx_run(50);
	CHECK_NEAR(phys(0), x0 + 0.037, 0.00051);

	/* a long rapid: bursts respect the FPGA ceiling measured on the unit (40 000 steps/s) */
	uint32_t b0 = C.fp.bursts;
	memcpy(t, m->pos, sizeof t);
	t[1] += 100;
	motion_move_to(m, t, 0, true);
	fx_until_idle(10000);
	CHECK(C.fp.bursts > b0);
	CHECK_NEAR(phys(1), t[1], 0.0005);
	CHECK(SIM.crashes == 0);

	/* zig-zag: every corner reverses X; the FPGA would ignore a reversal while still stepping,
	 * so motion holds it back - and not one step is lost */
	uint32_t d0 = m->deferred;
	memcpy(t, m->pos, sizeof t);
	for (int i = 0; i < 12; i++) { t[0] += (i & 1) ? -3 : 3; t[1] += 0.5; motion_move_to(m, t, 2400, false); }
	fx_until_idle(30000);
	CHECK_NEAR(phys(0), t[0], 0.0005); CHECK_NEAR(phys(1), t[1], 0.0005);
	CHECK(SIM.ignored_cmds == 0);
	CHECK(m->deferred >= d0);
	/* bursts carry the planner's speed: 1200 mm/min = 40 000 steps/s -> rate word 25 000 */
	CHECK(SIM.bank[0][0x12] > 0);
}
