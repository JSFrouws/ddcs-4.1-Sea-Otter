/* Controller + state machine against the simulated machine: guards, homing, limits,
 * E-stop, touch-off, programs (run, TryCut, simulate, multi-part, resume), remote start. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "t.h"
#include <sys/stat.h>
#include <unistd.h>
#include "core/machine.h"
#include "core/macro.h"
#include "sim/fpga_sim.h"

static const char *PROG =
	"G21 G90 G54\nG0 Z5\nG0 X0 Y0\nM3 S10000\nG1 Z-1 F300\nG1 X20 F1200\nG1 Y10\nG2 X10 Y20 I-10 J0\nG0 Z5\nM5\nM30\n";

static arg_t J(int axis, int dir, double d) { return (arg_t){ .axis = axis, .dir = dir, .dist = d }; }

static void load(const char *src)
{
	arg_t a = { .path = "test.nc", .text = src, .len = strlen(src) };
	CHECK(act(C_LOAD, &a, SRC_PANEL));
}

/* 2026-10-31 on the machine: steps/mm 500 -> 2000 over the API, drives on, IDLE, and emit()
 * streamed pos * new - steps as bursts: X +336.6 mm at full speed, Y, Z too. */
static void test_scale_safety(void)
{
	SUITE("steps/mm change, runaway guard, level gate");
	fx_reset();
	CHECK(act(C_HOME, NULL, SRC_PANEL));
	fx_until_idle(60000);
	CHECK(C.homed);
	int64_t sim0[NAXES]; memcpy(sim0, SIM.pos, sizeof sim0);
	double spm0 = SET.ax[AX_X].steps_per_mm;

	/* 1. a new scale while idle with the drives on: not one step, position follows the motors */
	for (int a = 0; a < 3; a++) SET.ax[a].steps_per_mm = spm0 * 4;
	ctl_apply_axis_config();
	fx_run(2000);
	for (int a = 0; a < NAXES; a++) CHECK(SIM.pos[a] == sim0[a]);
	for (int a = 0; a < 3; a++) CHECK_NEAR(C.mo.pos[a], (double)C.mo.steps[a] / (spm0 * 4), 1e-9);
	CHECK(!C.homed && C.m == M_IDLE && C.pos_faults == 0);
	for (int a = 0; a < 3; a++) SET.ax[a].steps_per_mm = spm0;
	ctl_apply_axis_config();
	fx_run(500);
	for (int a = 0; a < NAXES; a++) CHECK(SIM.pos[a] == sim0[a]);

	/* the same while a jog runs: it stops where it is, nothing jumps */
	arg_t j = { .axis = AX_X, .dir = 1, .dist = 20 };
	CHECK(act(C_JOG, &j, SRC_PANEL));
	fx_run(300);
	int64_t mid = SIM.pos[AX_X];
	SET.ax[AX_X].steps_per_mm = spm0 * 4;
	fx_run(1000);
	CHECK(llabs(SIM.pos[AX_X] - mid) < 2000);        /* at most the burst already in flight */
	CHECK(motion_idle(&C.mo));
	SET.ax[AX_X].steps_per_mm = spm0;
	fx_run(200);

	/* 2. a delta no plan can produce is refused, not sent */
	memcpy(sim0, SIM.pos, sizeof sim0);
	C.mo.pos[AX_Y] += 100;                            /* 100 mm out of nowhere */
	fx_run(500);
	CHECK(SIM.pos[AX_Y] == sim0[AX_Y]);
	CHECK_NEAR(C.mo.pos[AX_Y], (double)C.mo.steps[AX_Y] / spm0, 1e-9);
	CHECK(C.mo.runaway == 0);                         /* reported and cleared by the controller */

	/* 3a. E-stop: a pending delta is held back at the level, whatever put it there */
	SIM.ext_estop = true;
	fx_run(100);
	CHECK(C.m == M_ESTOP);
	uint32_t b0 = C.mo.blocked;
	C.mo.pos[AX_X] += 1;
	motion_tick(&C.mo, 0.010);                        /* even if something ticks motion anyway */
	fx_run(100);
	CHECK(SIM.pos[AX_X] == sim0[AX_X] && C.mo.blocked > b0);
	SIM.ext_estop = false;
	fx_run(100);
	CHECK(C.m == M_IDLE);

	/* 3b. a tripped switch: that direction held at the level, the way off still free */
	C.lim_active[AX_Z] = true; C.lim_dir[AX_Z] = 1;
	int64_t z0 = SIM.pos[AX_Z];
	b0 = C.mo.blocked;
	C.mo.pos[AX_Z] += 0.5;
	motion_tick(&C.mo, 0.010);
	CHECK(SIM.pos[AX_Z] == z0 && C.mo.blocked > b0);
	C.mo.pos[AX_Z] -= 0.5;
	motion_tick(&C.mo, 0.010);
	fx_run(100);
	CHECK(SIM.pos[AX_Z] < z0);
	C.lim_active[AX_Z] = false; C.lim_dir[AX_Z] = 0;
	fx_run(200);
}

/* 2026-10-31: jogs and the wheel on S-curves, exact distances; axes on any connector; enable */
static void test_jog_ramp_outputs(void)
{
	SUITE("jog / wheel ramps, drive outputs, drive enable, ports");
	fx_reset();
	const double spm = SET.ax[AX_X].steps_per_mm, A = SET.ax[AX_X].accel;

	/* 50 wheel clicks of 1 mm in one burst: exactly 50 mm, speed and accel within the axis */
	int64_t x0 = SIM.pos[AX_X];
	for (int i = 0; i < 50; i++) motion_nudge(&C.mo, AX_X, 1.0);
	double vmax = 0, amax = 0, last_v = 0;
	for (int t = 0; t < 20000 && !motion_idle(&C.mo); t += 10) {
		fx_run(10);
		vmax = fmax(vmax, fabs(C.mo.jv[AX_X]));
		double ac = fabs(C.mo.jv[AX_X] - last_v) / 0.010;
		amax = fmax(amax, ac);
		last_v = C.mo.jv[AX_X];
	}
	fx_run(100);
	CHECK(SIM.pos[AX_X] - x0 == (int64_t)llround(50 * spm));
	CHECK(vmax > 1 && vmax <= SET.ax[AX_X].max_vel / 60.0 + 1e-6);
	CHECK(amax <= A * 1.05);                            /* the last 10 ms tick lands at ~A */
	CHECK(C.pos_faults == 0);

	/* turned back while moving: +5 then -8 = -3 mm net, braking first, nothing lost */
	x0 = SIM.pos[AX_X];
	motion_nudge(&C.mo, AX_X, 5);
	fx_run(150);
	motion_nudge(&C.mo, AX_X, -8);
	fx_until_idle(20000); fx_run(100);
	CHECK(SIM.pos[AX_X] - x0 == (int64_t)llround(-3 * spm));

	/* continuous jog: ramps up (no full-speed first slice), released = brakes, runs on */
	x0 = SIM.pos[AX_Y];
	motion_jog_cont(&C.mo, AX_Y, 1);
	fx_run(10);
	CHECK(C.mo.jv[AX_Y] > 0 && C.mo.jv[AX_Y] < SET.jog_feed / 60.0 * 0.2);
	fx_run(1500);
	CHECK_NEAR(C.mo.jv[AX_Y], fmin(SET.jog_feed / 60.0, 32000 / spm), 1e-6);
	int64_t at_release = SIM.pos[AX_Y];
	motion_jog_cont(&C.mo, AX_Y, 0);
	CHECK(!motion_idle(&C.mo));                         /* still braking */
	fx_until_idle(5000); fx_run(100);
	CHECK(SIM.pos[AX_Y] > at_release);                  /* ran on */
	CHECK(C.pos_faults == 0 && C.fpos[AX_Y] == (int32_t)C.mo.steps[AX_Y]);

	/* X on the A connector: the A channel steps, the X channel stays put, no fault */
	const setting_desc_t *out = setting_find("axis.X.output");
	double v;
	CHECK(out && setting_parse_port(out, "A", &v) && v == 4);
	CHECK(setting_parse_port(out, "a (ap/ad)", &v) && v == 4);
	CHECK(setting_parse_port(out, "-", &v) && v == 0);
	CHECK(!setting_parse_port(out, "B", &v));
	CHECK(!strcmp(setting_port_name(out, 4), "A") && !strcmp(setting_port_name(out, 0), "-"));
	int64_t chx = SIM.pos[AX_X], cha = SIM.pos[AX_A];
	SET.ax[AX_A].output = 0;                            /* A gives its connector to X */
	bool rev_a = SIM.wiring_rev[AX_A];
	SIM.wiring_rev[AX_A] = SIM.wiring_rev[AX_X];        /* the X drive (and its wiring) moves with it */
	setting_set(out, 4); ctl_apply_axis_config();
	fx_run(100);
	arg_t j = { .axis = AX_X, .dir = 1, .dist = 2 };
	CHECK(act(C_JOG, &j, SRC_PANEL));
	fx_until_idle(5000); fx_run(100);
	CHECK(SIM.pos[AX_X] == chx && SIM.pos[AX_A] - cha == (int64_t)llround(2 * spm));
	CHECK(C.pos_faults == 0);
	/* "-": nothing is sent, the axis still counts */
	setting_set(out, 0); ctl_apply_axis_config();
	cha = SIM.pos[AX_A];
	double px = C.mo.pos[AX_X];
	CHECK(act(C_JOG, &j, SRC_PANEL));
	fx_until_idle(5000); fx_run(100);
	CHECK(SIM.pos[AX_X] == chx && SIM.pos[AX_A] == cha);
	CHECK_NEAR(C.mo.pos[AX_X], px + 2, 1e-6);
	CHECK(C.pos_faults == 0);
	setting_set(out, 1); SET.ax[AX_A].output = 4; SIM.wiring_rev[AX_A] = rev_a; ctl_apply_axis_config(); fx_run(100);
	CHECK(C.pos_faults == 0);

	/* drive enable: one output, on while the drives are on (low = OUT on) */
	SET.out_enable = 0;
	const setting_desc_t *en = setting_find("drives.enable_out");
	CHECK(en && setting_conflict(en, 1) != NULL);       /* OUT1 is the spindle */
	SET.out_aux = 0;
	CHECK(setting_conflict(en, 3) == NULL);
	SET.out_enable = 3; SET.enable_level = 0;
	fx_run(20);
	CHECK(SIM.outputs & 4);
	CHECK(act(C_DRIVES_OFF, NULL, SRC_PANEL) && C.m == M_OFF);
	fx_run(20);
	CHECK(!(SIM.outputs & 4));
	SET.enable_level = 1;
	fx_run(20);
	CHECK(SIM.outputs & 4);                             /* high while on = OUT on while off */
	CHECK(act(C_DRIVES_ON, NULL, SRC_PANEL) && C.m == M_IDLE);
	fx_run(20);
	CHECK(!(SIM.outputs & 4));
	SET.out_enable = 0; SET.out_aux = 3; SET.enable_level = 0;
	fx_run(20);
}

/* the real loop's slices come every 10-34 ms (the panel redraw); measured 2026-10-31. As in
 * main.c, a slice covers the time that has PASSED; the FPGA then runs for the next, unknown
 * interval - which is what made a burst still be running when the next slice came */
static void tick_jitter(int ms)
{
	static int passed = 10;
	ctl_tick(passed / 1000.0); sim_step(ms); clock_advance(ms);
	passed = ms;
}

/* 2026-10-31 trace on the machine: slices skipped while a burst still ran, then double -
 * 2x speed spikes and stops. Bursts now run at the planned speed: never much faster. */
static void test_pacing(void)
{
	SUITE("burst pacing under an irregular loop, braking time bound");
	fx_reset();
	SET.ax[AX_Y].steps_per_mm = 400;                     /* the machine: the FPGA has 5x headroom here */
	fx_run(50);
	const double spm = SET.ax[AX_Y].steps_per_mm, v = SET.jog_feed / 60.0;
	arg_t j = { .axis = AX_Y, .dir = 1, .dist = 20 };
	CHECK(act(C_JOG, &j, SRC_PANEL));
	double vmax = 0; int64_t y0 = SIM.pos[AX_Y], last = y0;
	int still = 0, stops = 0, t = 0;
	for (int k = 0; t < 8000 && !(motion_idle(&C.mo) && k > 5); k++) {
		int ms = k % 3 == 2 ? 34 : 10;
		tick_jitter(ms); t += ms;
		double sv = (double)(SIM.pos[AX_Y] - last) / spm / (ms / 1000.0);
		last = SIM.pos[AX_Y];
		double done = (double)(SIM.pos[AX_Y] - y0) / spm;
		if (done > 2 && done < 18) {                       /* the cruise, ramps excluded */
			vmax = fmax(vmax, sv);
			if (sv < 0.05 * v) { if (!still++) stops++; } else still = 0;
		}
	}
	fx_run(200);
	CHECK(SIM.pos[AX_Y] - y0 == (int64_t)llround(20 * spm));
	printf("  pacing: vmax %.2f mm/s (plan %.2f), stops %d, deferred %u\n", vmax, v, stops, C.mo.deferred);
	CHECK(vmax <= v * 1.3);                              /* was ~2.35x: a skipped slice sent double */
	CHECK(stops == 0);
	CHECK(C.pos_faults == 0);

	/* a 300 ms stall mid-jog (the FPGA busy, the plan going on): the backlog drains at no more
	 * than the axis' top speed + 15 %, and the move still lands exactly */
	y0 = SIM.pos[AX_Y];
	j.dist = 20;
	CHECK(act(C_JOG, &j, SRC_PANEL));
	fx_run(400);
	for (int k = 0; k < 30; k++) { ctl_tick(0.010); clock_advance(10); }   /* no sim_step: the axis stays busy */
	uint32_t peak = 0;
	for (int t2 = 0; t2 < 8000 && !motion_idle(&C.mo); t2 += 10) { fx_run(10); if (C.mo.rate[AX_Y] > peak) peak = C.mo.rate[AX_Y]; }
	fx_run(200);
	double top = SET.ax[AX_Y].max_vel / 60.0 * spm;
	CHECK(peak <= top * 1.15 + 52);                      /* the planned-rate rule: +15 % + 50 */
	CHECK(SIM.pos[AX_Y] - y0 == (int64_t)llround(20 * spm));
	CHECK(C.pos_faults == 0);

	/* a controlled stop that can't finish in time is cut dead, and says so */
	motion_jog_cont(&C.mo, AX_Y, -1);
	fx_run(1000);
	CHECK(C.mo.jv[AX_Y] < -0.5 * v);
	double acc = SET.ax[AX_Y].accel;
	motion_stop(&C.mo, false);                           /* brakes on its ramp ... */
	CHECK(C.mo.stopping);
	SET.ax[AX_Y].accel = 1;                              /* ... which now would take 20 s */
	fx_run(1500);
	CHECK(!C.mo.stopping && motion_idle(&C.mo) && C.mo.jv[AX_Y] == 0);
	CHECK(!C.mo.stop_cut);                               /* reported and cleared */
	SET.ax[AX_Y].accel = acc;
	fx_run(200);
	CHECK(C.pos_faults == 0);
}

void test_machine(void)
{
	test_scale_safety();
	test_jog_ramp_outputs();
	test_pacing();
	SUITE("state machine and controller vs simulated machine");
	fx_reset();
	SET.spindle_delay = 0;
	CHECK(C.m == M_IDLE);
	arg_t j = J(AX_X, 1, 1);

	/* --- every state's table: directional commands only where allowed --- */
	CHECK(can(C_JOG, &j, SRC_PANEL) == NULL);
	CHECK(can(C_START, NULL, SRC_PANEL) != NULL);          /* nothing loaded */
	load(PROG);
	CHECK(C.prog.loaded && C.prog.nlines == 11);
	CHECK(act(C_START, NULL, SRC_PANEL) && C.m == M_RUN);
	cmd_t motion_cmds[] = { C_JOG, C_TOUCHOFF, C_GOTOZERO, C_HOME, C_PROBE, C_SPINDLE, C_LOAD, C_EDIT_TABLE };
	for (size_t i = 0; i < ARRAY_LEN(motion_cmds); i++) {
		CHECK(can(motion_cmds[i], &j, SRC_PANEL) != NULL);
		CHECK(can(motion_cmds[i], &j, SRC_API) != NULL);
	}
	CHECK(can(C_OVERRIDE, NULL, SRC_PANEL) == NULL);
	CHECK(act(C_PAUSE, NULL, SRC_PANEL) && C.m == M_HOLD);
	for (size_t i = 0; i < ARRAY_LEN(motion_cmds); i++) CHECK(can(motion_cmds[i], &j, SRC_PANEL) != NULL);
	CHECK(act(C_START, NULL, SRC_PANEL) && C.m == M_RUN);
	for (int i = 0; i < 3000 && !C.spindle; i++) fx_run(10);
	fx_run(20);
	CHECK(C.spindle && SIM.outputs == 1 && SIM.pwm == 1250);  /* M3 S10000 -> OUT1 + 10000/24000*3000 */
	fx_until_idle(60000);
	CHECK(C.m == M_IDLE && !C.spindle && SIM.outputs == 0);
	CHECK_NEAR(ctl_work(AX_Z), 5, 0.001);
	CHECK_NEAR(sim_pos_mm(AX_X) - WD.wcs[0].off[0], 10, 0.001);  /* arc end X10 Y20 */
	CHECK_NEAR(sim_pos_mm(AX_Y) - WD.wcs[0].off[1], 20, 0.001);
	CHECK(C.work_done == C.prog.nlines - 1);
	/* the FPGA's own counters agree with every commanded step (checked at rest by ctl_tick) */
	fx_run(100);                                   /* 3 heartbeats at rest */
	if (C.pos_faults) printf("  pos faults %u, err %d %d %d\n", C.pos_faults, C.pos_err[0], C.pos_err[1], C.pos_err[2]);
	CHECK(C.pos_checks > 0 && C.pos_faults == 0);
	for (int a = 0; a < NAXES; a++) CHECK(C.fpos[a] == (int32_t)C.mo.steps[a]);

	/* --- feed override: rapid follows it down, caps at 100 --- */
	arg_t ov = { .kind = 0, .delta = 50 };
	act(C_OVERRIDE, &ov, SRC_PANEL);
	CHECK(C.mo.feed_ov == 150 && rapid_override(C.mo.feed_ov) == 100);
	ov.delta = -100;
	act(C_OVERRIDE, &ov, SRC_PANEL);
	CHECK(C.mo.feed_ov == 50 && rapid_override(C.mo.feed_ov) == 50);
	ov.delta = 50; act(C_OVERRIDE, &ov, SRC_PANEL);

	/* --- homing against the simulated switches: Z, X, Y --- */
	CHECK(act(C_HOME, NULL, SRC_PANEL) && C.m == M_HOMING);
	CHECK(can(C_JOG, &j, SRC_PANEL) != NULL);
	fx_until_idle(400000);
	CHECK(C.m == M_IDLE && C.homed);
	CHECK_NEAR(C.mo.pos[AX_X], 50, 1e-6);          /* switch = 0, backed off 50 */
	CHECK_NEAR(C.mo.pos[AX_Y], 50, 1e-6);
	CHECK_NEAR(C.mo.pos[AX_Z], -15, 1e-6);
	CHECK_NEAR(sim_pos_mm(AX_X), 50, 0.06);        /* machine frame now equals the physical one */
	CHECK_NEAR(sim_pos_mm(AX_Z), -15, 0.06);
	CHECK(SIM.crashes == 0);

	/* --- macros are files: the package's macro-*.nc plus the operator's own folder. Picking
	 * one arms it as the program on the Run page; Start runs it, Pause holds, Start goes on,
	 * Reset stops it - and the loaded program comes back afterwards --- */
	{
		snprintf(macro_dir_sys, sizeof macro_dir_sys, "../install/usb/ddcsv4");
		mkdir("umacros", 0755);
		FILE *f = fopen("umacros/nudge-x.nc", "w");
		fputs("(name: Nudge X)\n(info: my own macro)\nG91\nG0 X{max:2,1}\nG90\n", f);
		fclose(f);
		snprintf(macro_dir_user, sizeof macro_dir_user, "umacros");
		static macro_t ml[MACRO_MAX];
		int nm = macro_scan(ml, MACRO_MAX);
		bool park = false, mine = false;
		for (int k = 0; k < nm; k++) { park |= !strcmp(ml[k].name, "Park") && ml[k].needs_homed && !ml[k].user; mine |= !strcmp(ml[k].name, "Nudge X") && ml[k].user; }
		CHECK(nm >= 7 && park && mine);

		/* placeholders: settings, position, max/min; an unknown name is an error */
		char out[200], err[120];
		const char *src = "G0 Z{max:work.z,jog.safe_z} F{jog.feed}";
		CHECK(macro_expand(src, strlen(src), out, sizeof out, err, sizeof err) > 0);
		char want[120];
		snprintf(want, sizeof want, "G0 Z%.4f F%.4f", fmax(ctl_work(AX_Z), SET.safe_z), SET.jog_feed);
		CHECK(!strcmp(out, want));
		src = "G0 X{no.such.key}";
		CHECK(macro_expand(src, strlen(src), out, sizeof out, err, sizeof err) < 0 && strstr(err, "no.such.key"));

		char prog_name[128];
		snprintf(prog_name, sizeof prog_name, "%s", C.prog.name);
		double back[NAXES];
		memcpy(back, C.mo.pos, sizeof back);
		SET.park[0] = 60; SET.park[1] = 70; SET.park[2] = -12;
		arg_t m = { .path = "Park" };
		CHECK(act(C_MACRO, &m, SRC_PANEL) && C.macro_on && C.m == M_IDLE);   /* armed, nothing moves yet */
		CHECK(!strcmp(C.prog.name, "Park") && fabs(C.mo.pos[AX_X] - back[AX_X]) < 1e-9);
		CHECK(act(C_START, NULL, SRC_PANEL) && C.m == M_RUN);
		fx_until_idle(60000);
		CHECK(C.m == M_IDLE && !C.macro_on && !strcmp(C.prog.name, prog_name));   /* program back */
		CHECK_NEAR(C.mo.pos[AX_X], 60, 1e-6); CHECK_NEAR(C.mo.pos[AX_Y], 70, 1e-6); CHECK_NEAR(C.mo.pos[AX_Z], -12, 1e-6);

		/* warm-up: Pause holds it, Start continues, Reset stops it halfway */
		SET.warm_s = 4; SET.warm_rpm = 6000;
		m.path = "macro-warm-up.nc";                          /* by file name works too */
		CHECK(act(C_MACRO, &m, SRC_PANEL) && C.macro_on);
		CHECK(act(C_START, NULL, SRC_PANEL));
		fx_run(400);
		CHECK(C.m == M_RUN && C.spindle);
		CHECK(act(C_PAUSE, NULL, SRC_PANEL) && C.m == M_HOLD && C.macro_on);
		CHECK(act(C_START, NULL, SRC_PANEL) && C.m == M_RUN);
		fx_run(300);
		CHECK(act(C_STOP, NULL, SRC_PANEL) && C.m == M_IDLE);
		CHECK(!C.spindle && !C.macro_on && !strcmp(C.prog.name, prog_name));

		/* Reset before Start just cancels; the user's own macro from the share folder runs */
		m.path = "Nudge X";
		CHECK(act(C_MACRO, &m, SRC_PANEL) && C.macro_on);
		CHECK(act(C_STOP, NULL, SRC_PANEL) && !C.macro_on && !strcmp(C.prog.name, prog_name));
		double x0 = C.mo.pos[AX_X];
		CHECK(act(C_MACRO, &m, SRC_PANEL) && act(C_START, NULL, SRC_PANEL));
		fx_until_idle(30000);
		CHECK_NEAR(C.mo.pos[AX_X], x0 + 2, 1e-6);

		/* needs: homed - refused un-homed; and a macro whose rapid leaves the bench box is
		 * refused at Start (rapids count for the limit check, not only cutting moves) */
		C.homed = false;
		m.path = "Park";
		CHECK(can(C_MACRO, &m, SRC_PANEL) != NULL);
		C.homed = true;
		SET.bench_limits = true;
		for (int a = 0; a < 3; a++) { SET.ax[a].bench_min = C.mo.pos[a] - 5; SET.ax[a].bench_max = C.mo.pos[a] + 5; }
		CHECK(act(C_MACRO, &m, SRC_PANEL) && C.macro_on);
		const char *why = can(C_START, NULL, SRC_PANEL);
		CHECK(why && strstr(why, "bench limit"));
		CHECK(!act(C_START, NULL, SRC_PANEL) && C.m == M_IDLE);
		act(C_STOP, NULL, SRC_PANEL);
		SET.bench_limits = false;
		CHECK(!C.macro_on && !strcmp(C.prog.name, prog_name));
		remove("umacros/nudge-x.nc"); rmdir("umacros");

		motion_move_to(&C.mo, back, 0, true);                /* back where homing left it */
		fx_until_idle(60000);
		CHECK_NEAR(C.mo.pos[AX_X], back[AX_X], 1e-6);
	}

	/* --- soft limits after homing: a jog past the limit stops exactly AT it (the planner
	 * brakes into the endpoint), the next one that way is refused --- */
	arg_t sj = J(AX_X, -1, 55);
	CHECK(can(C_JOG, &sj, SRC_PANEL) == NULL);      /* 50 - 55 = -5 < soft min 0: clamped, not refused */
	CHECK(act(C_JOG, &sj, SRC_PANEL));
	fx_until_idle(20000);
	CHECK_NEAR(C.mo.pos[AX_X], SET.ax[AX_X].soft_min, 1e-6);
	CHECK(can(C_JOG, &sj, SRC_PANEL) != NULL);      /* at the limit: nothing more that way */
	arg_t back10 = J(AX_X, 1, 10);
	CHECK(act(C_JOG, &back10, SRC_PANEL));
	fx_until_idle(20000);
	CHECK_NEAR(C.mo.pos[AX_X], SET.ax[AX_X].soft_min + 10, 1e-6);

	/* --- config files: replaced in one step (never half-written by a power cut), and not
	 * rewritten at all when nothing changed (flash wear) --- */
	{
		struct stat a, b;
		CHECK(settings_save("t_settings.cfg") == 0 && workdata_save("t_work.cfg") == 0);
		stat("t_settings.cfg", &a);
		CHECK(settings_save("t_settings.cfg") == 0);
		stat("t_settings.cfg", &b);
		CHECK(a.st_ino == b.st_ino && a.st_mtime == b.st_mtime);    /* unchanged: untouched */
		double keep = SET.jog_feed; SET.jog_feed = keep + 1;
		CHECK(settings_save("t_settings.cfg") == 0);
		stat("t_settings.cfg", &b);
		CHECK(a.st_ino != b.st_ino);                                /* changed: a new file renamed over */
		CHECK(access("t_settings.cfg.new", F_OK) != 0);             /* nothing left behind */
		SET.jog_feed = 0; settings_load("t_settings.cfg");
		CHECK_NEAR(SET.jog_feed, keep + 1, 1e-9);
		SET.jog_feed = keep; settings_save("t_settings.cfg");
		stat("t_work.cfg", &a);
		CHECK(workdata_save("t_work.cfg") == 0);
		stat("t_work.cfg", &b);
		CHECK(a.st_ino == b.st_ino);
	}

	/* --- keep the homed state over a restart: off by default, and only from a position at rest --- */
	{
		settings_t d; settings_defaults(&d);
		CHECK(!d.retain_homed);
		CHECK(d.ax[AX_X].soft_max == 400 && d.ax[AX_Y].soft_max == 200 && d.ax[AX_Z].soft_min == -100);   /* 400 x 200 x 100 */
		CHECK(C.homed);
		double keep[NAXES]; memcpy(keep, C.mo.pos, sizeof keep);
		fx_run(2500);                                     /* 2 s at rest: written as rest + homed */
		fx_restart();
		CHECK(!C.homed);                                  /* the setting is off */
		SET.retain_homed = true; settings_save("t_settings.cfg");
		fx_restart();
		CHECK(C.homed && SET.retain_homed);
		for (int a = 0; a < NAXES; a++) CHECK_NEAR(C.mo.pos[a], keep[a], 1e-3);
		arg_t mv = J(AX_X, 1, 5);
		act(C_JOG, &mv, SRC_PANEL); fx_run(100);          /* moving: the file says so at once */
		fx_restart();
		CHECK(!C.homed);                                  /* "power cut" mid-move: not trusted */
		fx_until_idle(20000); fx_run(2500);
		fx_restart();
		CHECK(!C.homed);                                  /* and it stays lost until homed again */
		C.homed = true;                                   /* (as if re-homed, for the tests below) */
		SET.retain_homed = false; settings_save("t_settings.cfg");
	}

	/* --- Goto 0: Pause holds it, Start continues it, Reset ends it --- */
	{
		double s0[NAXES]; memcpy(s0, C.mo.pos, sizeof s0);
		CHECK(act(C_GOTOZERO, NULL, SRC_PANEL) && C.m == M_MOVE);
		fx_run(300);
		CHECK(act(C_PAUSE, NULL, SRC_PANEL) && C.m == M_HOLD && C.hold_move);
		fx_run(1500);
		double h[NAXES]; memcpy(h, C.mo.pos, sizeof h);
		fx_run(500);
		CHECK_NEAR(C.mo.pos[AX_Z], h[AX_Z], 1e-9);       /* standing still on hold */
		CHECK(act(C_START, NULL, SRC_PANEL) && C.m == M_MOVE);
		fx_until_idle(60000);
		CHECK(C.m == M_IDLE);
		CHECK_NEAR(ctl_work(AX_X), 0, 1e-6);
		CHECK_NEAR(ctl_work(AX_Y), 0, 1e-6);
		arg_t back = J(AX_X, 1, 5); act(C_JOG, &back, SRC_PANEL); fx_until_idle(20000);
		act(C_GOTOZERO, NULL, SRC_PANEL); fx_run(100); act(C_PAUSE, NULL, SRC_PANEL);
		CHECK(act(C_STOP, NULL, SRC_PANEL) && C.m == M_IDLE && !C.hold_move);
		fx_until_idle(20000);
		(void)s0;
	}

	/* --- Reset stops a jog in progress (jogs run in IDLE) --- */
	{
		double x0 = C.mo.pos[AX_X];
		arg_t lj = J(AX_X, 1, 40);
		act(C_JOG, &lj, SRC_PANEL);
		fx_run(500);
		CHECK(!motion_idle(&C.mo));
		CHECK(act(C_STOP, NULL, SRC_API));
		fx_until_idle(5000);
		CHECK(C.mo.pos[AX_X] - x0 < 20);
	}

	/* --- relative jogs sent faster than they run chain correctly (10 x +1/-1 = 0) --- */
	double y0 = C.mo.pos[AX_Y];
	for (int i = 0; i < 10; i++) { arg_t p = J(AX_Y, 1, 1), q = J(AX_Y, -1, 1); act(C_JOG, &p, SRC_API); act(C_JOG, &q, SRC_API); }
	fx_until_idle(30000);
	CHECK_NEAR(C.mo.pos[AX_Y], y0, 1e-9);

	/* --- touch-off with an edge Ø: X+ -> -6, same key again -> 0 --- */
	SET.edge_dia = 12;
	arg_t to = { .axis = AX_X, .dir = 1 };
	CHECK(act(C_TOUCHOFF, &to, SRC_PANEL));
	CHECK_NEAR(ctl_work(AX_X), -6, 1e-9);
	CHECK(act(C_TOUCHOFF, &to, SRC_PANEL));
	CHECK_NEAR(ctl_work(AX_X), 0, 1e-9);
	to.axis = AX_Y; to.dir = -1;
	act(C_TOUCHOFF, &to, SRC_PANEL);
	CHECK_NEAR(ctl_work(AX_Y), 6, 1e-9);
	to.axis = AX_Z; to.dir = -1;
	act(C_TOUCHOFF, &to, SRC_PANEL);
	CHECK_NEAR(ctl_work(AX_Z), 0, 1e-9);           /* Z ignores the Ø */
	SET.edge_dia = 0;

	/* --- goto zero: Z up to safe, then XY --- */
	sj = J(AX_Z, -1, 3); act(C_JOG, &sj, SRC_PANEL); fx_until_idle(5000);
	CHECK(act(C_GOTOZERO, NULL, SRC_PANEL) && C.m == M_MOVE);
	fx_until_idle(30000);
	CHECK(C.m == M_IDLE);
	CHECK_NEAR(ctl_work(AX_X), 0, 1e-6); CHECK_NEAR(ctl_work(AX_Y), 0, 1e-6); CHECK_NEAR(ctl_work(AX_Z), 5, 1e-6);

	/* --- a limit switch during a jog: only that direction blocked, clears itself --- */
	SIM.switch_at[AX_Y] = (int64_t)((C.mo.pos[AX_Y] - 2) * 2000);   /* pretend the switch is 2 mm away */
	sj = J(AX_Y, -1, 5); act(C_JOG, &sj, SRC_PANEL);
	fx_until_idle(5000);
	CHECK(C.m == M_LIMIT && C.limit_axis == AX_Y && C.limit_dir == -1);
	CHECK(can(C_JOG, &sj, SRC_PANEL) != NULL);     /* Y- blocked */
	arg_t ok1 = J(AX_X, 1, 1), ok2 = J(AX_Y, 1, 1);
	CHECK(can(C_JOG, &ok1, SRC_PANEL) == NULL);    /* other axes free */
	CHECK(can(C_START, NULL, SRC_PANEL) != NULL);
	CHECK(act(C_JOG, &ok2, SRC_PANEL));            /* the way off */
	fx_until_idle(5000);
	CHECK(C.m == M_IDLE);
	SIM.switch_at[AX_Y] = 0;

	/* --- each axis into its own switch: X-, Y-, Z+ (the switch sides of this machine).
	 * Controlled stop, only that direction blocked, sideways / back / up-down free,
	 * jogging back off clears it, and no step lost --- */
	{
		bool sl = SET.soft_limits; SET.soft_limits = false;
		const int dir[3] = { -1, -1, +1 };
		for (int ax = 0; ax < 3; ax++) {
			int d = dir[ax];
			SIM.switch_at[ax] = SIM.pos[ax] + d * 3000;                /* the switch 1.5 mm away */
			arg_t in = J(ax, d, 4);
			CHECK(act(C_JOG, &in, SRC_PANEL));
			fx_until_idle(8000);
			CHECK(C.m == M_LIMIT && C.lim_dir[ax] == d);
			CHECK(can(C_JOG, &in, SRC_PANEL) != NULL);               /* further in: blocked */
			for (int o = 0; o < 3; o++) if (o != ax) {                 /* the other axes, both ways */
				arg_t s1 = J(o, 1, 0.5), s2 = J(o, -1, 0.5);
				CHECK(can(C_JOG, &s1, SRC_PANEL) == NULL && can(C_JOG, &s2, SRC_PANEL) == NULL);
			}
			arg_t off = J(ax, -d, 3);
			CHECK(act(C_JOG, &off, SRC_PANEL));
			fx_until_idle(8000);
			CHECK(C.m == M_IDLE && C.lim_dir[ax] == 0);
			SIM.switch_at[ax] = 0;
		}
		fx_run(100);
		CHECK(C.pos_faults == 0);
		for (int k = 0; k < NAXES; k++) CHECK(C.fpos[k] == (int32_t)C.mo.steps[k]);

		/* two switches at the same moment: both stop the move and both block their way;
		 * releasing one leaves LIMIT on until the second is released too */
		arg_t yj = J(AX_Y, -1, 4);
		CHECK(act(C_JOG, &yj, SRC_PANEL));
		fx_run(150);
		SIM.switch_at[AX_X] = SIM.pos[AX_X] + 1;                        /* X- and Y- trip together */
		SIM.switch_at[AX_Y] = SIM.pos[AX_Y] + 1;
		fx_until_idle(8000);
		CHECK(C.m == M_LIMIT && C.lim_dir[AX_X] != 0 && C.lim_dir[AX_Y] == -1);
		SIM.switch_at[AX_Y] = 0; fx_run(30);                            /* Y released, X still on */
		CHECK(C.m == M_LIMIT && C.lim_dir[AX_Y] == 0 && C.lim_dir[AX_X] != 0);
		SIM.switch_at[AX_X] = 0; fx_run(30);
		CHECK(C.m == M_IDLE);
		SET.soft_limits = sl;
	}

	/* --- bench limits: a work box enforced even un-homed. A continuous jog slows down on
	 * the way in (v <= sqrt(2 a d)) and comes to a standstill exactly on the edge --- */
	{
		SET.bench_limits = true;
		for (int a = 0; a < 3; a++) { SET.ax[a].bench_min = C.mo.pos[a] - 50; SET.ax[a].bench_max = C.mo.pos[a] + 50; }
		SET.ax[AX_X].bench_max = C.mo.pos[AX_X] + 3;
		arg_t cj = { .axis = AX_X, .dir = 1, .cont = true };
		CHECK(act(C_JOG, &cj, SRC_PANEL));
		double last = C.mo.pos[AX_X], vprev = 1e9, vin = 0;
		bool slowing = true;
		for (int i = 0; i < 400 && C.mo.jog_dir; i++) {
			fx_run(10);
			double v = (C.mo.pos[AX_X] - last) / 0.01; last = C.mo.pos[AX_X];
			if (SET.ax[AX_X].bench_max - C.mo.pos[AX_X] < 0.5) { if (v > vprev + 1e-6) slowing = false; if (!vin) vin = v; }
			vprev = v;
		}
		CHECK(!C.mo.jog_dir);                                           /* it stopped by itself */
		CHECK_NEAR(C.mo.pos[AX_X], SET.ax[AX_X].bench_max, 0.001);
		CHECK(slowing);                                                 /* never sped up in the last 0.5 mm */
		printf("  bench edge: arrived within %.4f mm, last-0.5mm speed %.1f mm/s (jog %.1f)\n",
			fabs(C.mo.pos[AX_X] - SET.ax[AX_X].bench_max), vin, SET.jog_feed / 60);
		CHECK(can(C_JOG, &cj, SRC_PANEL) != NULL);                     /* at the edge: refused */
		fx_until_idle(5000); fx_run(100);
		CHECK(C.pos_faults == 0);

		/* a program that leaves the box is refused (Run, TryCut); Simulate still runs */
		load("G21 G90 G54\nG0 X0 Y0 Z1\nG1 X500 F1000\nM30\n");
		const char *why = can(C_START, NULL, SRC_PANEL);
		CHECK(why && strstr(why, "Program goes past the X+"));
		if (why) printf("  refused: %s\n", why);
		C.armed = ARM_SIM;
		CHECK(can(C_START, NULL, SRC_PANEL) == NULL);
		C.armed = ARM_RUN;
		SET.bench_limits = false;
		load(PROG);
	}

	/* --- E-stop from the pendant: everything stops, release -> IDLE, drives stay on --- */
	act(C_SPINDLE, NULL, SRC_PANEL);
	SIM.pendant_estop = true; fx_run(20);
	CHECK(C.m == M_ESTOP && !C.spindle && SIM.outputs == 0 && SIM.pwm == 0);
	CHECK(can(C_DRIVES_ON, NULL, SRC_PANEL) != NULL);
	SIM.pendant_estop = false; fx_run(20);
	CHECK(C.m == M_IDLE);                           /* user: the E-stop is for motion, not power */
	CHECK(can(C_JOG, &j, SRC_PANEL) == NULL);

	/* --- controlled stop (motion.graceful_stop, default on): an E-stop mid-move brakes along
	 * the path at the axis' deceleration - no lost steps, overshoot ~ v^2/2a; the drives stay
	 * on. With the setting off the stop is immediate (only the burst in flight). --- */
	for (int graceful = 1; graceful >= 0; graceful--) {
		SET.graceful_stop = graceful;
		arg_t gj = J(AX_X, 1, 30);
		CHECK(act(C_JOG, &gj, SRC_PANEL));
		fx_run(400);
		double v = C.mo.v, x0 = C.mo.pos[AX_X], a = SET.ax[AX_X].accel;
		CHECK(v > 1);
		SIM.pendant_estop = true; fx_run(20);
		CHECK(C.m == M_ESTOP);
		for (int i = 0; i < 500 && !motion_idle(&C.mo); i++) fx_run(10);
		double over = fabs(C.mo.pos[AX_X] - x0), want = v * v / (2 * a);
		printf("  %s stop at %.0f mm/s: overshoot %.3f mm (v^2/2a %.3f)\n", graceful ? "controlled" : "immediate", v, over, want);
		if (graceful) CHECK(over > 0.5 * want && over < 2 * want + 0.5);
		else CHECK(over < 0.5);
		SIM.pendant_estop = false; fx_run(100);
		CHECK(C.m == M_IDLE);
		CHECK(C.pos_faults == 0);
		for (int k = 0; k < NAXES; k++) CHECK(C.fpos[k] == (int32_t)C.mo.steps[k]);
	}
	SET.graceful_stop = true;

	/* --- TryCut: spindle stays off; Z goes to the clearance level once and never changes --- */
	C.armed = ARM_TRYCUT;
	act(C_START, NULL, SRC_PANEL);
	double minz = 1e9, maxz = -1e9;
	bool at_clear = false;
	for (int i = 0; i < 6000 && C.m == M_RUN; i++) {
		fx_run(10);
		if (fabs(ctl_work(AX_Z) - SET.safe_z) < 1e-6) at_clear = true;
		if (at_clear) { minz = fmin(minz, ctl_work(AX_Z)); maxz = fmax(maxz, ctl_work(AX_Z)); }
		CHECK(!C.spindle || 0); if (C.spindle) break;
	}
	CHECK(C.m == M_IDLE && at_clear && fabs(minz - SET.safe_z) < 1e-6 && fabs(maxz - SET.safe_z) < 1e-6);

	/* --- Simulate: no pulses at all, position restored --- */
	C.armed = ARM_SIM;
	uint32_t b0 = SIM.bursts; double xs = C.mo.pos[AX_X];
	act(C_START, NULL, SRC_PANEL);
	fx_until_idle(60000);
	CHECK(SIM.bursts == b0 && C.mo.pos[AX_X] == xs && C.m == M_IDLE);

	/* --- Simulate, then Reset in the middle of a move: still nothing may move, before or
	 * after the stop (found on the unit 2026-10-21: the braking simulated move kept going
	 * once real output was back on and drove the FPGA to the simulated position) --- */
	{
		load("G21 G90 G54\nG0 X0 Y0 Z5\nG1 X200 F3000\nM30\n");
		int64_t p0[NAXES];
		for (int k = 0; k < NAXES; k++) p0[k] = SIM.pos[k];
		uint32_t b1 = SIM.bursts;
		double keep[NAXES];
		memcpy(keep, C.mo.pos, sizeof keep);
		C.armed = ARM_SIM;
		CHECK(act(C_START, NULL, SRC_PANEL));
		fx_run(1500);                                        /* mid-move, at speed */
		CHECK(C.m == M_RUN && fabs(C.mo.pos[AX_X] - keep[AX_X]) > 1);
		CHECK(act(C_STOP, NULL, SRC_PANEL));
		fx_run(2000);                                        /* long after the stop */
		CHECK(SIM.bursts == b1);
		for (int k = 0; k < NAXES; k++) CHECK(SIM.pos[k] == p0[k]);
		for (int k = 0; k < NAXES; k++) CHECK_NEAR(C.mo.pos[k], keep[k], 1e-9);
		CHECK(C.m == M_IDLE && !C.mo.dry && motion_idle(&C.mo));
		load(PROG);
	}
	C.armed = ARM_RUN;

	/* --- stop mid-program, resume from a line --- */
	act(C_START, NULL, SRC_PANEL);
	fx_run(1500);
	CHECK(C.m == M_RUN);
	act(C_STOP, NULL, SRC_PANEL);
	fx_until_idle(5000);
	CHECK(C.m == M_IDLE && C.work_done >= 1);
	C.resume_line = 6;                                   /* "G1 Y10" */
	act(C_START, NULL, SRC_PANEL);
	CHECK(C.gc.spindle == 3 && C.gc.feed == 1200);      /* modal state rebuilt */
	fx_until_idle(60000);
	CHECK(C.m == M_IDLE);
	CHECK_NEAR(ctl_work(AX_Y), 20, 0.001);

	/* --- multi-part job: G54 and G55 both marked, the program runs on each --- */
	memcpy(WD.wcs[1].off, WD.wcs[0].off, sizeof WD.wcs[1].off);
	WD.wcs[1].off[AX_X] += 40;    /* inside the 400 mm X travel */
	WD.wcs[0].job = WD.wcs[1].job = true;
	act(C_START, NULL, SRC_PANEL);
	fx_until_idle(120000);
	CHECK(C.m == M_IDLE);
	CHECK_NEAR(C.mo.pos[AX_X] - WD.wcs[1].off[AX_X], 10, 1e-6);       /* finished on the second part */
	CHECK_NEAR(sim_pos_mm(AX_X), C.mo.pos[AX_X], 0.002);             /* sim = machine, within the switch's few-step hysteresis */
	WD.wcs[1].job = false;

	/* --- tool change by hand: Z up, M_TOOL (jog + touch-off allowed), Start asks, Start goes --- */
	{
		const char *tp = "G21 G90 G54\nT1 M6\nG0 X5 Y5\nG1 Z-1 F600\nG1 X10\nG0 Z3\nT2 M6\nG0 X20 Y5\nG1 Z-1 F600\nG1 X25\nG0 Z3\nM30\n";
		arg_t ld = { .path = "tools.nc", .text = tp, .len = strlen(tp) };
		act(C_LOAD, &ld, SRC_PANEL);
		CHECK(C.prog.loaded && C.prog.ntc == 2 && C.prog.tc_line[0] == 1 && C.prog.tc_line[1] == 6 && C.prog.tc_tool[1] == 2);
		SET.tc_mode = 0; SET.tc_where = 0; SET.tc_return_pct = 50;
		WD.loaded_tool = 1;                                 /* T1 already in: the first M6 doesn't stop */
		for (int i = 0; i < NWCS; i++) WD.wcs[i].job = false;
		WD.active = 0;
		act(C_START, NULL, SRC_PANEL);
		int t = 0;
		while (C.m == M_RUN && t < 60000) { fx_run(10); t += 10; }
		CHECK(C.m == M_TOOL && C.tc_tool == 2);
		CHECK_NEAR(ctl_work(AX_Z), SET.safe_z, 1e-6);         /* went up to the safe height */
		CHECK_NEAR(ctl_work(AX_X), 10, 1e-6);                 /* and stayed above the part */
		CHECK(!C.spindle);
		/* jog while changing; touch off Z with the new (0.5 mm longer) tool */
		double zoff = WD.wcs[0].off[AX_Z];
		arg_t j = { .axis = AX_Z, .dir = -1, .dist = 2 };
		CHECK(act(C_JOG, &j, SRC_PANEL));
		fx_until_idle(5000);
		CHECK(C.m == M_TOOL);
		double zm = C.mo.pos[AX_Z];
		WD.wcs[0].off[AX_Z] = zm - (SET.safe_z - 2 + 0.5);   /* = a touch-off with a 0.5 mm shorter tool */
		CHECK(act(C_START, NULL, SRC_PANEL) && C.m == M_TOOL && C.tc_confirm);   /* first Start: asks */
		j.dir = 1; act(C_JOG, &j, SRC_PANEL); fx_until_idle(5000);
		CHECK(!C.tc_confirm);                                /* moved: asks again */
		act(C_START, NULL, SRC_PANEL);
		CHECK(C.tc_confirm);
		CHECK(act(C_START, NULL, SRC_PANEL) && C.m == M_RUN);
		CHECK(WD.loaded_tool == 2);
		t = 0;
		while (C.m == M_RUN && t < 60000) { fx_run(10); t += 10; }
		CHECK(C.m == M_IDLE);
		CHECK_NEAR(ctl_work(AX_X), 25, 1e-6);
		CHECK_NEAR(ctl_work(AX_Z), 3, 1e-6);
		/* Reset in M_TOOL stops the program */
		WD.loaded_tool = 1; act(C_START, NULL, SRC_PANEL);
		t = 0; while (C.m == M_RUN && t < 60000) { fx_run(10); t += 10; }
		CHECK(C.m == M_TOOL);
		CHECK(act(C_STOP, NULL, SRC_PANEL) && C.m == M_IDLE && !C.feed_wait && C.work_done >= 0);
		fx_until_idle(5000);
		WD.wcs[0].off[AX_Z] = zoff;

		/* --- multi-part job per tool: G54 + G55, two tools -> T1 on both, ONE change, T2 on both --- */
		memcpy(WD.wcs[1].off, WD.wcs[0].off, sizeof WD.wcs[1].off);
		WD.wcs[1].off[AX_X] += 40;    /* inside the 400 mm X travel */
		WD.wcs[0].job = WD.wcs[1].job = true;
		WD.loaded_tool = 1;
		C.resume_line = -1; C.work_done = -1;
		act(C_START, NULL, SRC_PANEL);
		int changes = 0;
		double x_at_change = 0, max_x1 = -1e9;
		t = 0;
		while ((C.m == M_RUN || C.m == M_TOOL) && t < 120000) {
			max_x1 = fmax(max_x1, C.mo.pos[AX_X] - WD.wcs[1].off[AX_X]);
			if (C.m == M_TOOL) {
				changes++;
				x_at_change = C.mo.pos[AX_X] - WD.wcs[1].off[AX_X];
				CHECK(C.job_idx == 0 && C.job_seg == 2 && C.tc_tool == 2);   /* segments: header, T1, T2 */
				act(C_START, NULL, SRC_PANEL); act(C_START, NULL, SRC_PANEL);
			}
			fx_run(10); t += 10;
		}
		CHECK(C.m == M_IDLE && changes == 1);
		CHECK_NEAR(x_at_change, 10, 1e-6);         /* T1 had finished on the SECOND part before the change */
		CHECK_NEAR(max_x1, 25, 1e-6);

		/* the same job, one part at a time: all tools on G54, then all on G55 -> 3 changes */
		SET.job_order = 1; WD.loaded_tool = 1; C.resume_line = -1; C.work_done = -1;
		act(C_START, NULL, SRC_PANEL);
		int ch2 = 0, order2[4], n2 = 0;
		t = 0;
		while ((C.m == M_RUN || C.m == M_TOOL) && t < 160000) {
			if (C.m == M_TOOL) {
				if (n2 < 4) order2[n2++] = C.job_idx * 10 + C.tc_tool;
				ch2++; act(C_START, NULL, SRC_PANEL); act(C_START, NULL, SRC_PANEL);
			}
			fx_run(10); t += 10;
		}
		CHECK(C.m == M_IDLE && ch2 == 3);
		CHECK(n2 == 3 && order2[0] == 2 && order2[1] == 11 && order2[2] == 12);   /* G54 T2, G55 T1, G55 T2 */
		SET.job_order = 0;
		CHECK_NEAR(C.mo.pos[AX_X] - WD.wcs[1].off[AX_X], 25, 1e-6);      /* T2 finished on the second part */

		/* --- tool change by macro: toolchange.nc in the macro folder, {tool} filled in --- */
		snprintf(macro_dir_user, sizeof macro_dir_user, "%s", ".");
		FILE *mf = fopen("toolchange.nc", "w");
		if (mf) { fputs("(tool {tool} was {tool.prev})\nG91 G0 Z1\nG90\n", mf); fclose(mf); }
		SET.tc_mode = 1; WD.loaded_tool = 1;
		WD.wcs[1].job = false;
		act(C_START, NULL, SRC_PANEL);
		t = 0; bool tool_state = false;
		while (C.m == M_RUN && t < 60000) { fx_run(10); t += 10; tool_state |= C.m == M_TOOL; }
		CHECK(C.m == M_IDLE && !tool_state && WD.loaded_tool == 2);
		CHECK_NEAR(ctl_work(AX_X), 25, 1e-6);
		remove("toolchange.nc"); macro_dir_user[0] = 0;
		SET.tc_mode = 0;
	}

	/* --- remote start: off by default (user 2026-10-24: whatever can hurt someone at the
	 * machine is an explicit choice), allowed once switched on, refused again when off --- */
	{
		settings_t d;
		settings_defaults(&d);
		CHECK(!d.remote_start && !d.api_jog && !d.api_moves && !d.api_spindle && !d.api_coolant && !d.api_keys && !d.api_drives);
		CHECK(d.api && d.api_programs && d.api_offsets && d.api_tools && d.api_settings && d.api_overrides && d.api_panel);
	}
	CHECK(can(C_START, NULL, SRC_API) != NULL);
	SET.remote_start = true;
	CHECK(can(C_START, NULL, SRC_API) == NULL);
	SET.remote_start = false;
	CHECK(!act(C_START, NULL, SRC_API) && C.m == M_IDLE);
	SET.remote_start = true;
	SET.api = false;
	CHECK(can(C_START, NULL, SRC_API) != NULL);
	SET.api = true;

	/* --- program errors are reported with the line --- */
	arg_t bad = { .path = "bad.nc", .text = "G0 X1\nG42 X2\n", .len = 13 };
	act(C_LOAD, &bad, SRC_PANEL);
	CHECK(!C.prog.loaded && C.prog.err_line == 1);
}
