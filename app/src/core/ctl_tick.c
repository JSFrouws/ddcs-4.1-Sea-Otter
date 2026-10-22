/* Controller periodic work: inputs (E-stop, limits), program feeding (incl. TryCut
 * and multi-part jobs), motion events, outputs, remote-start countdown, agent card. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"

static bool level_active(int port, bool active_high)
{
	if (port <= 0 || port > 18) return false;
	bool high = (C.inputs >> (port - 1)) & 1u;
	return high == active_high;
}

static bool switch_active(int axis) { return level_active(SET.ax[axis].home_port, SET.limit_level_high); }

void ctl_apply_axis_config(void)
{
	fpga_axis_cfg_t cfg[NAXES];
	for (int a = 0; a < NAXES; a++) {
		const axis_set_t *s = &SET.ax[a];
		/* this machine shares one switch per axis for limit and home (axis.*.limit_port) */
		cfg[a] = (fpga_axis_cfg_t){ .limit_port_pos = (uint8_t)s->home_port, .limit_port_neg = (uint8_t)s->home_port,
			.home_port = (uint8_t)s->home_port, .alarm_port = (uint8_t)s->alarm_port,
			.dir_invert = s->dir_invert, .pulse_high = s->pulse_high };
	}
	fpga_init(&C.fp, cfg);
}

static void load_pos(double p[NAXES])
{
	const double photo[NAXES] = { 464.207, 274.491, -91.201, -1.0 };   /* last known (vendor screen) */
	memcpy(p, photo, sizeof photo);
	char path[300];
	snprintf(path, sizeof path, "%s.pos", C.wd_path);
	FILE *f = fopen(path, "r");
	if (!f) return;
	if (fscanf(f, "%lf %lf %lf %lf", &p[0], &p[1], &p[2], &p[3]) != 4) memcpy(p, photo, sizeof photo);
	fclose(f);
}

void ctl_save(void)
{
	settings_save(C.cfg_path);
	workdata_save(C.wd_path);
	char path[300];
	snprintf(path, sizeof path, "%s.pos", C.wd_path);
	FILE *f = fopen(path, "w");
	if (f) { fprintf(f, "%.4f %.4f %.4f %.4f\n", C.mo.pos[0], C.mo.pos[1], C.mo.pos[2], C.mo.pos[3]); fclose(f); }
}

void ctl_init(bus_t *bus, bool sim, const char *cfg_path, const char *wd_path)
{
	memset(&C, 0, sizeof C);
	C.sim = sim; C.cfg_path = cfg_path; C.wd_path = wd_path;
	settings_defaults(&SET);
	settings_load(cfg_path);
	workdata_defaults(&WD);
	workdata_load(wd_path);
	fpga_attach(&C.fp, bus);
	ctl_apply_axis_config();
	/* machine position: the FPGA's own counters if it has any (they survive app restarts while
	 * the unit is powered - the vendor app restores them at boot too), else our saved file */
	double p[NAXES];
	load_pos(p);
	fpga_heartbeat(&C.fp);
	fpga_positions(&C.fp, C.fpos);
	bool have = false;
	for (int a = 0; a < NAXES; a++) have |= C.fpos[a] != 0;
	for (int a = 0; a < NAXES; a++) {
		if (have) p[a] = C.fpos[a] / SET.ax[a].steps_per_mm;
		else fpga_preset(&C.fp, a, (int32_t)llround(p[a] * SET.ax[a].steps_per_mm));
	}
	motion_init(&C.mo, &C.fp, p);
	C.mo.switch_active = switch_active;
	C.mo.range = ctl_axis_range;
	C.resume_line = C.work_done = -1; C.touch_axis = -1; C.limit_axis = -1;
	C.rpm = SET.spindle_default;
	C.m = M_IDLE;
	C.inputs = fpga_inputs(&C.fp);
	for (int a = 0; a < NAXES; a++) {
		C.lim_active[a] = switch_active(a);
		C.lim_dir[a] = C.lim_active[a] ? 2 : 0;
		if (C.lim_active[a] && C.m == M_IDLE) {          /* active since power-up: direction unknown */
			C.m = M_LIMIT; C.limit_axis = a; C.limit_dir = 0;
			ctl_toast(true, "%c limit input active at start-up - check the switch, or its level in Settings", AXIS_CH[a]);
		}
	}
}

/* ---- program feeding ---- */
static void next_part_or_end(void)
{
	if (C.job_idx >= 0) {
		int n = -1;
		for (int i = C.job_idx + 1; i < NWCS; i++) if (WD.wcs[i].job) { n = i; break; }
		if (n >= 0) {                                       /* safe Z, then the same program on the next part */
			double p[NAXES];
			memcpy(p, C.gc.pos, sizeof p);
			p[AX_Z] = fmax(p[AX_Z], fmax(WD.wcs[C.job_idx].off[AX_Z], WD.wcs[n].off[AX_Z]) + SET.safe_z);
			motion_move_to(&C.mo, p, 0, true);
			C.job_idx = n;
			gc_begin(&C.gc, C.prog.text, C.prog.len, p);
			C.gc.force_wcs = n; C.gc.apply_g10 = C.run_mode != ARM_SIM;
			ctl_toast(false, "Next part: G%d", 54 + n);
			return;
		}
	}
	gc_op_t e = { .kind = OP_END, .line = C.prog.nlines - 1 };
	if (motion_push(&C.mo, &e)) C.feeding = false;
}

static void feed_program(void)
{
	gc_op_t op;
	int budget = 200;                                   /* ops per tick: a deep refill never stalls a slice */
	while (C.feeding && motion_space(&C.mo) > 4 && motion_wants_more(&C.mo) && budget-- > 0) {
		int r = gc_next(&C.gc, &op);
		if (r < 0) {
			ctl_toast(true, "Line %d: %s", C.gc.err_line + 1, C.gc.err);
			act(C_STOP, NULL, SRC_MACHINE);
			return;
		}
		if (r == 0 || op.kind == OP_END) { next_part_or_end(); continue; }
		if (C.run_mode == ARM_TRYCUT) {                     /* dry run: spindle off, Z fixed */
			if (op.kind == OP_SPINDLE) continue;
			/* user: TryCut stays at the clearance level above the work plane and never changes Z */
			if (op.kind == OP_LINE) op.end[AX_Z] = ctl_trycut_z();
		}
		motion_push(&C.mo, &op);
	}
}

static void motion_events(void)
{
	mev_t ev = C.mo.ev;
	gc_op_t op = C.mo.ev_op;
	if (ev == MEV_NONE) return;
	if (ev == MEV_END && C.mo.hw_busy) return;        /* finished only when the FPGA is too */
	C.mo.ev = MEV_NONE;
	arg_t a = { 0 };
	switch (ev) {
	case MEV_SPINDLE:
		if (C.run_mode == ARM_RUN) { C.spindle = op.ival != 0; if (op.value > 0) C.rpm = op.value; }
		break;
	case MEV_COOLANT: if (C.run_mode != ARM_SIM) C.coolant = op.ival; break;
	case MEV_PAUSE:
		if (op.ival == PAUSE_TOOL) snprintf(C.pause_msg, sizeof C.pause_msg, "M6: change to T%d, then Start", (int)op.value);
		else snprintf(C.pause_msg, sizeof C.pause_msg, "M%d: program pause - Start continues", op.ival == PAUSE_M1 ? 1 : 0);
		if (op.ival == PAUSE_TOOL && op.value >= 1 && op.value <= NTOOLS) WD.loaded_tool = (int)op.value;
		act(C_PAUSE, NULL, SRC_MACHINE);
		ctl_toast(false, "%s", C.pause_msg);
		break;
	case MEV_END: act(C_DONE, NULL, SRC_MACHINE); break;
	case MEV_HOMED: a.dir = 1; act(C_DONE, &a, SRC_MACHINE); break;
	case MEV_HOME_FAIL: a.dir = 0; act(C_DONE, &a, SRC_MACHINE); break;
	default: break;
	}
}

static void inputs(void)
{
	C.inputs = fpga_inputs(&C.fp);
	C.pend = fpga_pendant(&C.fp);
	if (C.m == M_ESTOP || C.m == M_OFF) C.mo.hw_busy = C.mo.dry ? 0 : fpga_moving(&C.fp);
	bool ext = level_active(SET.estop_port, SET.estop_level_high), pend = SET.pendant_estop && C.pend.estop;
	bool es = ext || pend;
	if (es && C.m != M_ESTOP) {
		snprintf(C.stop_cause, sizeof C.stop_cause, ext && pend ? "pendant E-stop + external E-stop (IN%02d)"
			: pend ? "the pendant E-stop" : "the external E-stop (IN%02d)", SET.estop_port);
		act(C_ESTOP_IN, NULL, SRC_MACHINE);
	}
	if (!es && C.m == M_ESTOP) act(C_ESTOP_OUT, NULL, SRC_MACHINE);
	/* every axis on its own: two switches at once both stop and both block their direction;
	 * LIMIT only clears when the last one is released */
	for (int a = 0; a < NAXES; a++) {
		bool on = switch_active(a);
		if (C.m != M_HOMING) {
			if (on && !C.lim_active[a]) {
				arg_t x = { .axis = a, .dir = C.mo.last_dir[a] };
				if (C.m == M_IDLE || C.m == M_MOVE || C.m == M_RUN || C.m == M_LIMIT) act(C_LIMIT_HIT, &x, SRC_MACHINE);
				else C.lim_dir[a] = x.dir ? x.dir : 2;          /* HOLD / E-stop / off: remember it */
			}
			if (!on && C.lim_active[a]) {
				C.lim_dir[a] = 0;
				bool any = false;
				for (int k = 0; k < NAXES; k++) any |= C.lim_dir[k] != 0;
				if (!any && C.m == M_LIMIT) act(C_LIMIT_CLEAR, NULL, SRC_MACHINE);
			}
		}
		C.lim_active[a] = on;
	}
}

static void outputs(void)
{
	uint8_t m = 0;
	if (C.spindle && SET.out_spindle) m |= (uint8_t)(1u << (SET.out_spindle - 1));
	if ((C.coolant & 1) && SET.out_flood) m |= (uint8_t)(1u << (SET.out_flood - 1));
	if ((C.coolant & 4) && SET.out_aux) m |= (uint8_t)(1u << (SET.out_aux - 1));
	fpga_set_outputs(&C.fp, m);
	static int last = -1;
	int duty = C.spindle ? (int)lround(C.rpm * C.mo.spin_ov / 100.0 / SET.spindle_max * 3000) : 0;
	duty = CLAMP(duty, 0, 3000);
	if (duty != last) { fpga_spindle_pwm(&C.fp, duty); last = duty; }
}

static void agent_checks(void)
{
	agent_t *g = &C.agent;
	for (int i = 0; i < g->n; i++) {
		const char *k = g->step[i].check;
		if (!strcmp(k, "homed")) g->step[i].done = C.homed;
		else if (!strncmp(k, "loaded:", 7)) g->step[i].done = C.prog.loaded && !strcmp(C.prog.name, k + 7);
		else if (!strncmp(k, "touched:", 8)) {
			bool all = true;
			for (const char *p = k + 8; *p; p++)
				for (int a = 0; a < NAXES; a++) if (*p == AXIS_CH[a] && !C.touched[a]) all = false;
			g->step[i].done = all;
		} else if (!strcmp(k, "running")) g->step[i].done |= C.m == M_RUN;
	}
}

void ctl_tick(double dt)
{
	static uint64_t hb;
	inputs();
	if (C.mo.soft_stop) {                               /* a jog braked to a standstill at its limit */
		ctl_toast(false, "%c stopped at its limit", AXIS_CH[C.mo.soft_stop - 1]);
		C.mo.soft_stop = 0;
	}

	if (C.m == M_RUN) feed_program();
	if ((C.m != M_ESTOP && C.m != M_OFF) || C.mo.stopping) motion_tick(&C.mo, dt);   /* a controlled stop brakes on in E-stop */
	motion_events();
	if (C.m == M_MOVE && motion_idle(&C.mo)) act(C_DONE, NULL, SRC_MACHINE);
	outputs();
	if (C.countdown > 0 && now_ms() - C.countdown_t >= 1000) {
		C.countdown_t = now_ms();
		if (--C.countdown == 0) act(C_START, NULL, SRC_API);
	}
	agent_checks();
	/* watchdog: without it the FPGA accepts bursts but never steps (measured); it also latches
	 * the position counters, which we compare with the commanded steps whenever all is at rest */
	if (now_ms() - hb >= 20) {
		hb = now_ms();
		fpga_heartbeat(&C.fp);
		fpga_positions(&C.fp, C.fpos);
		/* compare only after everything has been at rest for 3 heartbeats in a row (so the
		 * latched counters are current); then the counter is the truth - it is what stepped */
		static int settle;
		bool rest = motion_idle(&C.mo) && !C.mo.dry && C.m != M_ESTOP && C.m != M_HOMING && !fpga_moving(&C.fp);
		settle = rest ? settle + 1 : 0;
		if (settle == 3) {
			C.pos_checks++;
			for (int a = 0; a < NAXES; a++) {
				int32_t e = (int32_t)(C.fpos[a] - (int32_t)C.mo.steps[a]);
				C.pos_err[a] = e;
				if (!e) continue;
				C.pos_faults++; C.fault_line = C.mo.line * 100 + (int)C.m;
				ctl_toast(true, "%c position check: FPGA counter differs by %d steps - re-synced", AXIS_CH[a], e);
				motion_set_pos(&C.mo, a, C.fpos[a] / SET.ax[a].steps_per_mm);
			}
		}
	}
}
