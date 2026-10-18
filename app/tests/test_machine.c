/* Controller + state machine against the simulated machine: guards, homing, limits,
 * E-stop, touch-off, programs (run, TryCut, simulate, multi-part, resume), remote start. */
#include <math.h>
#include <string.h>
#include "t.h"
#include "core/machine.h"
#include "sim/fpga_sim.h"

static const char *PROG =
	"G21 G90 G54\nG0 Z5\nG0 X0 Y0\nM3 S10000\nG1 Z-1 F300\nG1 X20 F1200\nG1 Y10\nG2 X10 Y20 I-10 J0\nG0 Z5\nM5\nM30\n";

static arg_t J(int axis, int dir, double d) { return (arg_t){ .axis = axis, .dir = dir, .dist = d }; }

static void load(const char *src)
{
	arg_t a = { .path = "test.nc", .text = src, .len = strlen(src) };
	CHECK(act(C_LOAD, &a, SRC_PANEL));
}

void test_machine(void)
{
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

	/* --- macros: park (machine position, Z up first), warm-up (spindle on, then off) --- */
	{
		double back[NAXES];
		memcpy(back, C.mo.pos, sizeof back);
		SET.park[0] = 60; SET.park[1] = 70; SET.park[2] = -12;
		arg_t m = { .kind = MAC_PARK };
		CHECK(act(C_MACRO, &m, SRC_PANEL) && C.m == M_MOVE);
		fx_until_idle(60000);
		CHECK(C.m == M_IDLE);
		CHECK_NEAR(C.mo.pos[AX_X], 60, 1e-6); CHECK_NEAR(C.mo.pos[AX_Y], 70, 1e-6); CHECK_NEAR(C.mo.pos[AX_Z], -12, 1e-6);
		SET.warm_s = 1; SET.warm_rpm = 6000;
		m.kind = MAC_WARMUP;
		CHECK(act(C_MACRO, &m, SRC_PANEL));
		fx_run(200);
		CHECK(C.spindle && C.m == M_MOVE);
		fx_run(1200);
		CHECK(!C.spindle && C.m == M_IDLE);
		motion_move_to(&C.mo, back, 0, true);          /* back where homing left it */
		fx_until_idle(60000);
		CHECK_NEAR(C.mo.pos[AX_X], back[AX_X], 1e-6);
	}

	/* --- soft limits after homing --- */
	arg_t sj = J(AX_X, -1, 45);
	CHECK(can(C_JOG, &sj, SRC_PANEL) != NULL);      /* 50 - 45 = 5 < soft min 10 */
	sj.dist = 30;
	CHECK(act(C_JOG, &sj, SRC_PANEL));
	fx_until_idle(20000);
	CHECK_NEAR(C.mo.pos[AX_X], 20, 1e-6);

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
	WD.wcs[1].off[AX_X] += 100;
	WD.wcs[0].job = WD.wcs[1].job = true;
	act(C_START, NULL, SRC_PANEL);
	fx_until_idle(120000);
	CHECK(C.m == M_IDLE);
	CHECK_NEAR(C.mo.pos[AX_X] - WD.wcs[1].off[AX_X], 10, 1e-6);       /* finished on the second part */
	CHECK_NEAR(sim_pos_mm(AX_X), C.mo.pos[AX_X], 0.002);             /* sim = machine, within the switch's few-step hysteresis */
	WD.wcs[1].job = false;

	/* --- remote start: allowed by default, refused once the owner switches it off --- */
	{
		settings_t d;
		settings_defaults(&d);
		CHECK(d.remote_start);
	}
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
