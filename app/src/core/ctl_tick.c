/* Controller periodic work: inputs (E-stop, limits), program feeding (incl. TryCut
 * and multi-part jobs), motion events, outputs, remote-start countdown, agent card. */
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "machine.h"
#include "macro.h"

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

/* The position file "<work>.pos": x y z a rest homed. It is rewritten 2 s after the machine
 * comes to rest (rest = 1) and marked rest = 0 once when it starts moving, so after a power
 * cut it is either the exact position at rest, or known to be stale. */
static char resume_path[256]; static int resume_at;   /* the program line from the position file */

static int load_pos(double p[NAXES], int *homed)
{
	const double photo[NAXES] = { 464.207, 274.491, -91.201, -1.0 };   /* last known (vendor screen) */
	memcpy(p, photo, sizeof photo);
	*homed = 0;
	char path[300];
	snprintf(path, sizeof path, "%s.pos", C.wd_path);
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	int rest = 0;
	int n = fscanf(f, "%lf %lf %lf %lf %d %d", &p[0], &p[1], &p[2], &p[3], &rest, homed);
	resume_path[0] = 0; resume_at = 0;
	if (n == 6 && fscanf(f, " prog %d %255[^\n]", &resume_at, resume_path) != 2) resume_path[0] = 0;
	fclose(f);
	if (n < 4) { memcpy(p, photo, sizeof photo); return 0; }
	if (n < 6) { *homed = 0; rest = 1; }                 /* an older file: position only */
	return rest;
}

static void pos_write(bool rest)
{
	char path[300], tmp[310];
	snprintf(path, sizeof path, "%s.pos", C.wd_path);
	snprintf(tmp, sizeof tmp, "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fprintf(f, "%.4f %.4f %.4f %.4f %d %d\n", C.mo.pos[0], C.mo.pos[1], C.mo.pos[2], C.mo.pos[3], rest, rest && C.homed);
	/* and roughly where the program was (user 2026-10-24: after a power cut, come back there):
	 * the line running, or the resume point; 0 = just the program */
	const program_t *pg = C.macro_on ? &C.saved_prog : &C.prog;
	if (pg->loaded && pg->path[0] == '/') {
		int line = C.macro_on ? C.saved_resume : (C.m == M_RUN || C.m == M_HOLD || C.m == M_TOOL) ? C.mo.line : C.resume_line;
		fprintf(f, "prog %d %s\n", line > 0 ? line : 0, pg->path);
	}
	fflush(f); fsync(fileno(f)); fclose(f);
	rename(tmp, path);                                   /* never a half-written file */
}

static void pos_track(void)
{
	static int written = -1, homed_w = -1, resume_w = -2;   /* 1 rest written, 0 moving written */
	static const char *text_w;
	static uint64_t rest_since;
	bool rest = motion_idle(&C.mo) && !C.mo.dry && C.m != M_HOMING && C.m != M_ESTOP;
	static uint64_t run_w;                               /* while a program runs: its line once a minute */
	if (C.m == M_RUN && now_ms() - run_w > 60000) { run_w = now_ms(); pos_write(false); written = 0; }
	if (!rest) {
		rest_since = 0;
		if (written != 0) { pos_write(false); written = 0; }
		return;
	}
	if (!rest_since) rest_since = now_ms();
	if ((written != 1 || homed_w != C.homed || resume_w != C.resume_line || text_w != C.prog.text) && now_ms() - rest_since > 2000) {
		pos_write(true); written = 1; homed_w = C.homed; resume_w = C.resume_line; text_w = C.prog.text;
	}
}

void ctl_save(void)
{
	settings_save(C.cfg_path);
	workdata_save(C.wd_path);
	pos_write(motion_idle(&C.mo) && !C.mo.dry);
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
	int was_homed, rest = load_pos(p, &was_homed);
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
	/* user 2026-10-23: optionally keep "homed" over a restart. The drives were reset too (a
	 * small clunk): expect ~1/100-1/50 turn, 0.05-0.1 mm on a 5 mm screw - re-home for precision */
	if (SET.retain_homed && was_homed && rest) {
		C.homed = true;
		ctl_toast(false, "Homed state kept from before the restart (Settings) - re-home for full accuracy");
	} else if (SET.retain_homed && was_homed) ctl_toast(true, "Not homed: the power went off while the machine was moving");
	/* the program it was on, and roughly where (never started by itself) */
	if (resume_path[0] && program_load(&C.prog, resume_path) == 0) {
		if (resume_at > 0 && resume_at < C.prog.nlines) {
			C.resume_line = resume_at; C.work_done = resume_at - 1;
			ctl_toast(false, "Back after a restart: %s, Start continues near line %d - check it first", C.prog.name, resume_at + 1);
		} else ctl_toast(false, "Back after a restart: %s loaded", C.prog.name);
	}
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
static void move_if(const double p[NAXES], double feed, bool rapid)
{
	double q[NAXES], d = 0;
	motion_queued_end(&C.mo, q);
	for (int a = 0; a < NAXES; a++) d = fmax(d, fabs(p[a] - q[a]));
	if (d > 1e-9) motion_move_to(&C.mo, p, feed, rapid);
}

/* to a program position (machine) the safe way: Z up to the safe level of both work
 * offsets, across, then down - used between parts and after a skipped / macro tool change */
static void approach(const double to[NAXES], int wcs_from, int wcs_to)
{
	double p[NAXES];
	motion_queued_end(&C.mo, p);
	double t[NAXES];
	memcpy(t, to, sizeof t);
	if (C.run_mode == ARM_TRYCUT) t[AX_Z] = ctl_trycut_z();
	p[AX_Z] = fmax(p[AX_Z], fmax(WD.wcs[wcs_from].off[AX_Z], WD.wcs[wcs_to].off[AX_Z]) + SET.safe_z);
	move_if(p, 0, true);
	p[AX_X] = t[AX_X]; p[AX_Y] = t[AX_Y]; p[AX_A] = t[AX_A];
	move_if(p, 0, true);
	bool above = t[AX_Z] >= WD.wcs[wcs_to].off[AX_Z];  /* above the work zero: rapid, below: feed */
	move_if(t, above ? 0 : fmax(C.gc.feed, 1), above);
}

/* multi-part job: the program's tool segments (program_t.tc_line) */
static int seg_first_line(int s) { return s > 0 ? C.prog.tc_line[s - 1] : 0; }
static int seg_end_line(int s) { return s < C.prog.ntc ? C.prog.tc_line[s] : INT_MAX; }
static int job_after(int i) { for (int k = i + 1; k < NWCS; k++) if (WD.wcs[k].job) return k; return -1; }

/* part n, tool segment s: safe Z, rebuild the program state at the segment's first line,
 * go there. The first part of a new segment starts AT its M6 line, so the tool change runs
 * once (and returns to this part); the other parts start just after it. */
static void start_part(int s, int n)
{
	bool new_tool = s != C.job_seg;
	int from = C.job_idx;
	double p[NAXES];
	motion_queued_end(&C.mo, p);
	p[AX_Z] = fmax(p[AX_Z], fmax(WD.wcs[from].off[AX_Z], WD.wcs[n].off[AX_Z]) + SET.safe_z);
	move_if(p, 0, true);
	gc_begin(&C.gc, C.prog.text, C.prog.len, p);
	C.gc.force_wcs = n; C.gc.apply_g10 = C.run_mode != ARM_SIM;
	int line = seg_first_line(s) + (s > 0 && !new_tool ? 1 : 0);
	if (line > 0) {
		gc_seek(&C.gc, line);
		if (!new_tool || SET.tc_mode == 2) approach(C.gc.pos, from, n);   /* else the tool change goes there */
	}
	C.job_idx = n; C.job_seg = s;
	if (new_tool) ctl_toast(false, "Next tool (T%d): parts again from G%d", C.prog.tc_tool[s - 1], 54 + n);
	else ctl_toast(false, "Next part: G%d%s", 54 + n, C.prog.ntc ? " (same tool)" : "");
}

static void next_part_or_end(void)
{
	if (C.job_idx >= 0) {
		int n = job_after(C.job_idx), s = C.job_seg;
		if (n < 0 && s < C.prog.ntc) { n = job_after(-1); s++; }   /* every part has had this tool */
		if (n >= 0) { start_part(s, n); return; }
	}
	gc_op_t e = { .kind = OP_END, .line = C.prog.nlines - 1 };
	if (motion_push(&C.mo, &e)) C.feeding = false;
}

/* ---- tool change at an M6 (reached by the motion, not the feeder: positions are real) ---- */
static bool tc_macro_path(int t, char *out, size_t n)
{
	const char *dirs[2] = { macro_dir_user, macro_dir_sys };
	for (int k = 0; k < 2; k++) {                       /* per tool first, then the general one */
		if (!dirs[k][0]) continue;
		snprintf(out, n, "%.200s/toolchange-T%d.nc", dirs[k], t);
		if (!access(out, R_OK)) return true;
	}
	for (int k = 0; k < 2; k++) {
		if (!dirs[k][0]) continue;
		snprintf(out, n, "%.200s/toolchange.nc", dirs[k]);
		if (!access(out, R_OK)) return true;
	}
	return false;
}

static void tool_change(int t)
{
	motion_hold(&C.mo, false);                          /* the motion stops itself at a pause op */
	C.tc_tool = t;
	for (int k = 0; k < NWCS; k++) memcpy(C.tc_off[k], WD.wcs[k].off, sizeof C.tc_off[k]);
	int w = ctl_prog_wcs();
	if (SET.tc_mode == 2 || t == WD.loaded_tool || C.run_mode == ARM_SIM) {
		ctl_toast(false, C.run_mode == ARM_SIM ? "M6 T%d: simulated, no stop" : t == WD.loaded_tool ? "M6 T%d: already in" : "M6 T%d ignored (Settings > Tool change)", t);
		if (C.run_mode != ARM_SIM && t >= 1 && t <= NTOOLS) WD.loaded_tool = t;
		approach(C.gc.pos, w, w);                        /* a multi-part job may continue on another part */
		C.feed_wait = false;
		return;
	}
	C.spindle = false; C.coolant = 0;
	if (SET.tc_mode == 1) {
		char path[256], err[160];
		size_t len;
		if (!tc_macro_path(t, path, sizeof path)) ctl_toast(true, "No toolchange-T%d.nc or toolchange.nc in the macro folders - change by hand", t);
		else {
			char *text = macro_expand_file(path, &len, err, sizeof err);
			if (!text) ctl_toast(true, "Tool-change macro: %s - change by hand", err);
			else {
				free(C.tc_text);
				C.tc_text = text;
				double p[NAXES];
				motion_queued_end(&C.mo, p);
				gc_begin(&C.tc_gc, text, len, p);
				C.tc_gc.force_wcs = w;
				C.tc_state = TC_MACRO;
				const char *b = strrchr(path, '/');
				ctl_toast(false, "M6 T%d: running %s", t, b ? b + 1 : path);
				return;
			}
		}
	}
	/* by hand: Z up to the safe height, optionally to the tool-change position, then M_TOOL */
	double p[NAXES];
	memcpy(p, C.mo.pos, sizeof p);
	p[AX_Z] = fmax(p[AX_Z], WD.wcs[w].off[AX_Z] + SET.safe_z);
	if (SET.tc_where == 1 && C.homed) {
		p[AX_Z] = fmax(p[AX_Z], SET.tc[2]);
		move_if(p, 0, true);
		p[AX_X] = SET.tc[0]; p[AX_Y] = SET.tc[1];
		move_if(p, 0, true);
		p[AX_Z] = SET.tc[2];
	} else if (SET.tc_where == 1) ctl_toast(true, "Not homed - changing the tool here (the tool-change position is in machine coordinates)");
	move_if(p, 0, true);
	snprintf(C.pause_msg, sizeof C.pause_msg, "M6: going to change to T%d", t);
	C.tc_state = TC_GOING;
}

/* the tool-change macro runs as part of the program; afterwards back to the program */
static void feed_tc_macro(void)
{
	gc_op_t op;
	int budget = 200;
	while (motion_space(&C.mo) > 8 && motion_wants_more(&C.mo) && budget-- > 0) {
		int r = gc_next(&C.tc_gc, &op);
		if (r < 0) {
			ctl_toast(true, "Tool-change macro line %d: %s", C.tc_gc.err_line + 1, C.tc_gc.err);
			act(C_STOP, NULL, SRC_MACHINE);
			return;
		}
		if (r == 0 || op.kind == OP_END) {
			if (C.tc_tool >= 1 && C.tc_tool <= NTOOLS) WD.loaded_tool = C.tc_tool;
			workdata_save(C.wd_path);
			free(C.tc_text); C.tc_text = NULL;
			C.tc_state = TC_NONE;
			int w = ctl_prog_wcs();
			if (C.gc.spindle && C.run_mode == ARM_RUN) {
				gc_op_t s = { .kind = OP_SPINDLE, .ival = C.gc.spindle, .value = C.gc.rpm, .line = C.mo.line };
				motion_push(&C.mo, &s);
			}
			approach(C.gc.pos, w, w);
			C.feed_wait = false;
			return;
		}
		if (op.kind == OP_PAUSE && op.ival == PAUSE_TOOL) continue;   /* no M6 inside the M6 */
		op.line = C.mo.line >= 0 ? C.mo.line : op.line;   /* the listing stays on the program's M6 */
		motion_push(&C.mo, &op);
	}
}

static void feed_program(void)
{
	gc_op_t op;
	int budget = 200;                                   /* ops per tick: a deep refill never stalls a slice */
	if (C.tc_state == TC_MACRO) { feed_tc_macro(); return; }
	while (C.feeding && !C.feed_wait && motion_space(&C.mo) > 4 && motion_wants_more(&C.mo) && budget-- > 0) {
		int r = gc_next(&C.gc, &op);
		if (r < 0) {
			ctl_toast(true, "Line %d: %s", C.gc.err_line + 1, C.gc.err);
			act(C_STOP, NULL, SRC_MACHINE);
			return;
		}
		if (r == 0 || op.kind == OP_END) { next_part_or_end(); continue; }
		if (C.job_idx >= 0 && op.line >= seg_end_line(C.job_seg)) { next_part_or_end(); continue; }   /* next tool: other parts first */
		if (C.run_mode == ARM_TRYCUT) {                     /* dry run: spindle off, Z fixed */
			if (op.kind == OP_SPINDLE) continue;
			/* user: TryCut stays at the clearance level above the work plane and never changes Z */
			if (op.kind == OP_LINE) op.end[AX_Z] = ctl_trycut_z();
		}
		if (op.kind == OP_PAUSE && op.ival == PAUSE_TOOL && SET.tc_mode != 2) {
			if (motion_push(&C.mo, &op)) C.feed_wait = true;   /* wait: dealt with when the motion gets there */
			return;
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
		if (op.ival == PAUSE_TOOL) {
			if (SET.tc_mode == 2) { if (op.value >= 1 && op.value <= NTOOLS) WD.loaded_tool = (int)op.value; motion_hold(&C.mo, false); break; }
			tool_change((int)op.value);
			break;
		}
		snprintf(C.pause_msg, sizeof C.pause_msg, "M%d: program pause - Start continues", op.ival == PAUSE_M1 ? 1 : 0);
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
			: pend ? ((C.pend.raw & 0x3FF) == 0x3FF ? "the pendant E-stop - every pendant line reads high: unplugged, loose cable or no power?" : "the pendant E-stop")
			: "the external E-stop (IN%02d)", SET.estop_port);
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
				if (C.m == M_IDLE || C.m == M_MOVE || C.m == M_RUN || C.m == M_LIMIT || C.m == M_TOOL) act(C_LIMIT_HIT, &x, SRC_MACHINE);
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
	if (C.m == M_RUN && C.tc_state == TC_GOING && motion_idle(&C.mo) && C.mo.ev == MEV_NONE) {
		C.tc_state = TC_NONE;
		act(C_TOOL_READY, NULL, SRC_MACHINE);          /* at the change position: M_TOOL, jog allowed */
	}
	outputs();
	if (C.countdown > 0 && now_ms() - C.countdown_t >= 1000) {
		C.countdown_t = now_ms();
		if (--C.countdown == 0) act(C_START, NULL, SRC_API);
	}
	agent_checks();
	pos_track();
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
