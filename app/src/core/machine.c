#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "machine.h"
#include "macro.h"

ctl_t C;

const char *const STATE_NAMES[M__N] = { "ESTOP", "OFF", "IDLE", "MOVE", "HOMING", "PROBING", "RUN", "HOLD", "LIMIT", "TOOL" };
const char *const CMD_NAMES[C__N] = {
	"jog", "touchoff", "gotozero", "home", "probe", "start", "pause", "stop", "spindle",
	"drives_on", "drives_off", "load", "edit_table", "edit_setting", "override",
	"estop_in", "estop_out", "limit_hit", "limit_clear", "done", "macro", "coolant", "tool_ready" };

#define B(c) (1u << (c))
/* Which commands each state accepts. Everything else is refused with WHY[state]. */
static const uint32_t ALLOW[M__N] = {
	[M_ESTOP]   = B(C_ESTOP_OUT) | B(C_EDIT_TABLE) | B(C_EDIT_SETTING) | B(C_LOAD),
	[M_OFF]     = B(C_DRIVES_ON) | B(C_ESTOP_IN) | B(C_EDIT_TABLE) | B(C_EDIT_SETTING) | B(C_LOAD),
	[M_IDLE]    = B(C_JOG) | B(C_TOUCHOFF) | B(C_GOTOZERO) | B(C_HOME) | B(C_PROBE) | B(C_START) |
	              B(C_SPINDLE) | B(C_DRIVES_OFF) | B(C_LOAD) | B(C_EDIT_TABLE) | B(C_EDIT_SETTING) |
	              B(C_OVERRIDE) | B(C_ESTOP_IN) | B(C_LIMIT_HIT) | B(C_STOP) | B(C_MACRO) | B(C_COOLANT),
	[M_MOVE]    = B(C_PAUSE) | B(C_STOP) | B(C_ESTOP_IN) | B(C_LIMIT_HIT) | B(C_DONE) | B(C_OVERRIDE),
	[M_HOMING]  = B(C_PAUSE) | B(C_STOP) | B(C_ESTOP_IN) | B(C_DONE),
	[M_PROBING] = B(C_PAUSE) | B(C_STOP) | B(C_ESTOP_IN) | B(C_DONE),
	[M_RUN]     = B(C_PAUSE) | B(C_STOP) | B(C_OVERRIDE) | B(C_ESTOP_IN) | B(C_LIMIT_HIT) | B(C_DONE) | B(C_COOLANT) | B(C_TOOL_READY),
	[M_HOLD]    = B(C_START) | B(C_STOP) | B(C_OVERRIDE) | B(C_ESTOP_IN) | B(C_COOLANT),
	[M_LIMIT]   = B(C_JOG) | B(C_HOME) | B(C_STOP) | B(C_DRIVES_OFF) | B(C_ESTOP_IN) | B(C_LIMIT_CLEAR) | B(C_LIMIT_HIT) |
	              B(C_EDIT_TABLE) | B(C_EDIT_SETTING) | B(C_OVERRIDE) | B(C_LOAD),
	[M_TOOL]    = B(C_JOG) | B(C_TOUCHOFF) | B(C_START) | B(C_PAUSE) | B(C_STOP) | B(C_ESTOP_IN) | B(C_LIMIT_HIT) |
	              B(C_EDIT_TABLE) | B(C_OVERRIDE) | B(C_COOLANT),
};

static const char *const WHY[M__N] = {
	[M_ESTOP] = "E-stop active - release it to continue (the drives stay on)",
	[M_OFF] = "Drives are off - press RESET to switch them on",
	[M_IDLE] = "Nothing to pause",
	[M_MOVE] = "Move in progress - Reset stops it",
	[M_HOMING] = "Homing in progress - Reset stops it",
	[M_PROBING] = "Probing in progress - Reset stops it",
	[M_RUN] = "Program running - directional controls are locked (Pause, Reset, overrides)",
	[M_HOLD] = "On hold - Start resumes, Reset stops; jogging stays locked",
	[M_LIMIT] = "Limit switch active - jog off it first (other axes are free)",
	[M_TOOL] = "Tool change - jog / touch off, Start goes back to the program, Reset stops it",
};

bool ctl_busy(void) { return C.m == M_RUN || C.m == M_HOLD || C.m == M_HOMING || C.m == M_MOVE || C.m == M_PROBING; }

void ctl_toast(bool bad, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(C.msg, sizeof C.msg, fmt, ap);
	va_end(ap);
	C.msg_bad = bad; C.msg_t = now_ms();
}

double ctl_work(int a) { return C.mo.pos[a] - WD.wcs[WD.active].off[a]; }

int ctl_cur_line(void)
{
	if (C.m == M_RUN || C.m == M_HOLD) return C.mo.line;
	return C.work_done >= 0 ? C.work_done : 0;
}

/* The box an axis may move in: soft limits (only once homed - before that the machine
 * position means nothing) and the bench limits (a work box, enforced homed or not).
 * Returns false when neither is in force for that axis. */
bool ctl_axis_range(int a, double *lo, double *hi)
{
	if (a < 0 || a >= AX_A) return false;
	const axis_set_t *s = &SET.ax[a];
	bool on = false;
	*lo = -1e18; *hi = 1e18;
	if (C.homed && SET.soft_limits) { *lo = s->soft_min; *hi = s->soft_max; on = true; }
	if (SET.bench_limits) { *lo = fmax(*lo, s->bench_min); *hi = fmin(*hi, s->bench_max); on = true; }
	return on;
}

/* which of the two limits is the binding one for axis a, on the + (up) or - side */
static const char *range_name_for(int a, bool up)
{
	bool soft = C.homed && SET.soft_limits, bench = SET.bench_limits;
	if (!soft) return "bench";
	if (!bench) return "soft";
	const axis_set_t *s = &SET.ax[a];
	return (up ? s->bench_max <= s->soft_max : s->bench_min >= s->soft_min) ? "bench" : "soft";
}

/* already at (or past) the limit in that direction: nothing to jog */
static bool at_limit(int a, int dir)
{
	double lo, hi;
	if (!ctl_axis_range(a, &lo, &hi)) return false;
	return (dir > 0 && C.mo.pos[a] >= hi - 1e-6) || (dir < 0 && C.mo.pos[a] <= lo + 1e-6);
}

/* a target (machine coordinates) outside the box: why, or NULL */
static const char *target_outside(const double t[NAXES])
{
	static char why[128];
	for (int a = 0; a < AX_A; a++) {
		double lo, hi;
		if (!ctl_axis_range(a, &lo, &hi)) continue;
		if (t[a] > hi + 1e-6 || t[a] < lo - 1e-6) {
			snprintf(why, sizeof why, "%c would go to %.3f mm, outside the %s limits %.3f ... %.3f", AXIS_CH[a], t[a], range_name_for(a, t[a] > hi), lo, hi);
			return why;
		}
	}
	return NULL;
}

/* the loaded program, placed on every work offset it will run on, inside the box? */
static const char *program_outside(void)
{
	static char why[160];
	int ws[NWCS], n = 0;
	for (int i = 0; i < NWCS; i++) if (WD.wcs[i].job) ws[n++] = i;
	if (n < 2) { ws[0] = WD.active; n = 1; }
	for (int k = 0; k < n; k++) {
		const double *off = WD.wcs[ws[k]].off;
		for (int a = 0; a < AX_A; a++) {
			double lo, hi;
			if (!ctl_axis_range(a, &lo, &hi)) continue;
			if (C.prog.mlo[a] > C.prog.mhi[a]) continue;          /* no moves */
			double plo = C.prog.mlo[a] + off[a], phi = C.prog.mhi[a] + off[a];   /* rapids too */
			if (C.armed == ARM_TRYCUT && a == AX_Z) plo = phi = off[AX_Z] + SET.safe_z;   /* TryCut flies at one Z */
			if (phi > hi + 1e-6 || plo < lo - 1e-6) {
				bool up = phi > hi + 1e-6;
				snprintf(why, sizeof why, "Program goes past the %c%c %s limit on G%d: reaches %.3f mm, limit %.3f",
					AXIS_CH[a], up ? '+' : '-', range_name_for(a, up), 54 + ws[k], up ? phi : plo, up ? hi : lo);
				return why;
			}
		}
	}
	return NULL;
}

const char *can(cmd_t c, const arg_t *a, src_t src)
{
	static char why[128];
	static const arg_t none;
	if (!a) a = &none;
	if (src == SRC_API && !SET.api) return "API is switched off (Settings > Network & API)";
	if (!(ALLOW[C.m] & B(c))) return WHY[C.m];
	switch (c) {
	case C_START:
		if (src == SRC_API && !SET.remote_start) return "remote START is disabled on the panel (403)";
		if (C.m == M_IDLE && !C.prog.loaded) return "No program loaded - Files > Load";
		if (C.m == M_IDLE && !motion_idle(&C.mo)) return "Wait for the jog to finish";
		if (C.m == M_IDLE && C.armed != ARM_SIM) { const char *w = program_outside(); if (w) return w; }
		if (C.m == M_HOLD) for (int k = 0; k < NAXES; k++) if (C.lim_dir[k]) return "A limit switch is active - Reset, then jog off it";
		if (C.m == M_TOOL) {
			if (!motion_idle(&C.mo)) return "Wait for the jog to finish";
			double t[NAXES], via;
			ctl_tc_target(t, &via);
			t[AX_Z] = fmax(t[AX_Z], via);
			const char *w = target_outside(t);
			if (w) return w;
		}
		break;
	case C_JOG: {
		int ld = a->axis >= 0 && a->axis < NAXES ? C.lim_dir[a->axis] : 0;
		if ((ld == 1 || ld == -1) && a->dir == ld) {
			snprintf(why, sizeof why, "%c%c blocked by its limit switch - %c%c and the other axes are free",
				AXIS_CH[a->axis], a->dir > 0 ? '+' : '-', AXIS_CH[a->axis], a->dir > 0 ? '-' : '+');
			return why;
		}
		if (at_limit(a->axis, a->dir)) { snprintf(why, sizeof why, "%c%c is at its %s limit", AXIS_CH[a->axis], a->dir > 0 ? '+' : '-', range_name_for(a->axis, a->dir > 0)); return why; }
		break;
	}
	case C_TOUCHOFF:
		if (!motion_idle(&C.mo)) return "Wait for the jog to finish - touch-off needs the axis standing still";
		break;
	case C_PAUSE:
		if (C.m == M_TOOL && motion_idle(&C.mo)) return "Nothing to pause";
		break;
	case C_PROBE: return "No probe input is configured on this machine";
	case C_GOTOZERO: {
		if (!motion_idle(&C.mo)) return "Wait for the jog to finish";
		double t[NAXES];
		memcpy(t, C.mo.pos, sizeof t);
		const double *o = WD.wcs[WD.active].off;
		t[AX_Z] = fmax(t[AX_Z], o[AX_Z] + SET.safe_z);
		if (a->kind != 1) { t[AX_X] = o[AX_X]; t[AX_Y] = o[AX_Y]; }
		const char *w = target_outside(t);
		if (w) return w;
		break;
	}
	case C_MACRO: {
		static macro_t m;
		if (!a->path || macro_find(a->path, &m)) return "No such macro (see the Macros page)";
		if (!motion_idle(&C.mo)) return "Wait for the jog to finish";
		if (m.needs_homed && !C.homed) { snprintf(why, sizeof why, "%s: home first - it uses machine coordinates", m.name); return why; }
		break;
	}
	default: break;
	}
	return NULL;
}

static mstate_t next_state(cmd_t c, mstate_t f)
{
	switch (c) {
	case C_ESTOP_IN: return M_ESTOP;
	case C_ESTOP_OUT: return M_IDLE;   /* user: the E-stop is for unwanted motion, not power - drives stay on */
	case C_DRIVES_ON: return M_IDLE;
	case C_DRIVES_OFF: return M_OFF;
	case C_HOME: return M_HOMING;
	case C_PROBE: return M_PROBING;
	case C_GOTOZERO: return M_MOVE;
	case C_START: return f == M_HOLD && C.hold_move ? M_MOVE : M_RUN;
	case C_PAUSE: return f == M_RUN || f == M_MOVE ? M_HOLD : f == M_TOOL ? M_TOOL : M_IDLE;
	case C_TOOL_READY: return M_TOOL;
	case C_STOP: return f == M_LIMIT ? M_LIMIT : M_IDLE;
	case C_LIMIT_HIT: return M_LIMIT;
	case C_LIMIT_CLEAR: case C_DONE: return M_IDLE;
	default: return f;
	}
}

static double sim_saved_pos[NAXES];

static void all_off(void) { C.spindle = false; C.coolant = 0; }

/* no tool change in progress any more (program started, stopped, finished, E-stop, limit) */
static void tc_clear(void)
{
	C.tc_state = TC_NONE; C.tc_confirm = false; C.feed_wait = false;
	free(C.tc_text); C.tc_text = NULL;
}

int ctl_prog_wcs(void) { return C.gc.force_wcs >= 0 ? C.gc.force_wcs : C.gc.wcs; }

/* The program position the change returns to: where the program is (C.gc.pos - the feeder
 * waits at the M6, so it is exactly that line), moved by any work offset change made during
 * the change (a Z touch-off with the new tool), so the WORK position is the same. */
double ctl_tc_target(double t[NAXES], double *via_z)
{
	int w = ctl_prog_wcs();
	for (int a = 0; a < NAXES; a++) t[a] = C.gc.pos[a] + WD.wcs[w].off[a] - C.tc_off[w][a];
	if (C.run_mode == ARM_TRYCUT) t[AX_Z] = ctl_trycut_z();
	*via_z = fmax(t[AX_Z], WD.wcs[w].off[AX_Z] + SET.safe_z);
	double v = SET.ax[AX_X].max_vel;
	for (int a = 1; a < 3; a++) v = fmin(v, SET.ax[a].max_vel);
	return v * SET.tc_return_pct / 100.0;
}

/* Start (confirmed) in M_TOOL: the new tool is in - back to the program and on */
static void tc_resume(void)
{
	int w = ctl_prog_wcs();
	if (C.tc_tool >= 1 && C.tc_tool <= NTOOLS) WD.loaded_tool = C.tc_tool;
	/* a Z touch-off during the change is the new tool's length: the other parts of a
	 * multi-part job get the same shift (unless they were touched off themselves) */
	int src = -1;
	for (int k = 0; k < NWCS && src < 0; k++)
		if ((k == w || (C.job_idx >= 0 && WD.wcs[k].job)) && fabs(WD.wcs[k].off[AX_Z] - C.tc_off[k][AX_Z]) > 1e-9) src = k;
	int shifted = 0;
	if (src >= 0 && C.job_idx >= 0) {
		double dz = WD.wcs[src].off[AX_Z] - C.tc_off[src][AX_Z];
		for (int k = 0; k < NWCS; k++)
			if (WD.wcs[k].job && fabs(WD.wcs[k].off[AX_Z] - C.tc_off[k][AX_Z]) < 1e-9) { WD.wcs[k].off[AX_Z] += dz; shifted++; }
		if (shifted) ctl_toast(false, "New tool length: Z %+.3f mm applied to the %d other part%s of the job too", dz, shifted, shifted > 1 ? "s" : "");
	}
	workdata_save(C.wd_path);
	double t[NAXES], via;
	double v = ctl_tc_target(t, &via);
	memcpy(C.gc.pos, t, sizeof t);                      /* same work position, new offsets */
	for (int k = 0; k < NWCS; k++) memcpy(C.gc.wcs_off[k], WD.wcs[k].off, sizeof C.gc.wcs_off[k]);
	if (C.gc.spindle && C.run_mode == ARM_RUN) {        /* spindle back on before going back */
		gc_op_t s = { .kind = OP_SPINDLE, .ival = C.gc.spindle, .value = C.gc.rpm, .line = C.mo.line };
		motion_push(&C.mo, &s);
	}
	if (C.gc.coolant && C.run_mode != ARM_SIM) { gc_op_t s = { .kind = OP_COOLANT, .ival = C.gc.coolant }; motion_push(&C.mo, &s); }
	double p[NAXES];
	memcpy(p, t, sizeof p);
	p[AX_Z] = via;
	motion_move_to(&C.mo, p, v, false);                 /* user: in a straight line, at the return speed */
	if (via > t[AX_Z] + 1e-9) motion_move_to(&C.mo, t, v, false);
	C.tc_state = TC_NONE; C.tc_confirm = false; C.feed_wait = false;
	C.pause_msg[0] = 0;
	if (!shifted) ctl_toast(false, "T%d in - back to the program", C.tc_tool);
}

static void start_program(src_t src)
{
	gc_begin(&C.gc, C.prog.text, C.prog.len, C.mo.pos);
	C.gc.apply_g10 = C.armed != ARM_SIM;
	C.run_mode = C.armed;
	int jobs = 0, keep = C.job_idx;
	for (int i = 0; i < NWCS; i++) jobs += WD.wcs[i].job;
	C.job_idx = -1; C.job_seg = 0;
	tc_clear();
	if (jobs > 1) {                                   /* multi-part job: first JOB fixture */
		for (int i = 0; i < NWCS; i++) if (WD.wcs[i].job) { C.job_idx = i; break; }
		/* resuming: on the part that was being cut, in the tool segment of the resume line */
		if (C.resume_line > 0 && keep >= 0 && keep < NWCS && WD.wcs[keep].job) C.job_idx = keep;
		if (C.resume_line > 0) while (C.job_seg < C.prog.ntc && C.prog.tc_line[C.job_seg] <= C.resume_line) C.job_seg++;
		C.gc.force_wcs = C.job_idx;
	}
	C.mo.dry = C.armed == ARM_SIM;
	if (C.armed == ARM_SIM) { const char *w = program_outside(); if (w) ctl_toast(true, "Simulating anyway - %s", w); }
	if (C.mo.dry) memcpy(sim_saved_pos, C.mo.pos, sizeof sim_saved_pos);
	C.feeding = true;
	if (C.resume_line > 0) {                          /* rebuild modal state, then approach */
		gc_seek(&C.gc, C.resume_line);
		int w = C.gc.force_wcs >= 0 ? C.gc.force_wcs : C.gc.wcs;
		double p[NAXES];
		memcpy(p, C.mo.pos, sizeof p);
		p[AX_Z] = fmax(p[AX_Z], WD.wcs[w].off[AX_Z] + SET.safe_z);
		motion_move_to(&C.mo, p, 0, true);
		p[AX_X] = C.gc.pos[AX_X]; p[AX_Y] = C.gc.pos[AX_Y]; p[AX_A] = C.gc.pos[AX_A];
		motion_move_to(&C.mo, p, 0, true);
		if (C.gc.spindle && C.armed == ARM_RUN) {
			gc_op_t s = { .kind = OP_SPINDLE, .ival = C.gc.spindle, .value = C.gc.rpm, .line = C.resume_line };
			motion_push(&C.mo, &s);
		}
		if (C.gc.coolant) { gc_op_t s = { .kind = OP_COOLANT, .ival = C.gc.coolant }; motion_push(&C.mo, &s); }
		if (C.armed == ARM_TRYCUT) { memcpy(p, C.gc.pos, sizeof p); p[AX_Z] = ctl_trycut_z(); motion_move_to(&C.mo, p, C.gc.feed, false); }
		else motion_move_to(&C.mo, C.gc.pos, C.gc.feed, false);
		ctl_toast(false, "Restored state for line %d, approached at safe Z, continuing", C.resume_line + 1);
		C.resume_line = -1;
	} else {
		if (C.armed == ARM_TRYCUT) {                      /* straight up/down to the clearance level first */
			double p[NAXES];
			memcpy(p, C.mo.pos, sizeof p);
			p[AX_Z] = ctl_trycut_z();
			motion_move_to(&C.mo, p, 0, true);
		}
		ctl_toast(false, "%s%s", src == SRC_API ? "Started by agent - " : "",
			C.armed == ARM_RUN ? "Program started" : C.armed == ARM_TRYCUT ? "TryCut: spindle off, Z fixed at the clearance level" : "Simulating - no motion");
	}
}

/* TryCut height: the clearance level above the active work plane (machine coordinates) */
double ctl_trycut_z(void)
{
	int w = C.gc.force_wcs >= 0 ? C.gc.force_wcs : C.gc.wcs;
	return WD.wcs[w].off[AX_Z] + SET.safe_z;
}

/* ---- macros: a small program that takes C.prog's place while it is armed or running ---- */
static bool macro_load(const char *path, const char *name, program_t *out)
{
	char err[160];
	size_t len;
	char *text = macro_expand_file(path, &len, err, sizeof err);
	if (!text) { ctl_toast(true, "Macro %s: %s", name, err); return false; }
	memset(out, 0, sizeof *out);
	int r = program_load_text(out, name, text, len);
	free(text);
	if (r) { ctl_toast(true, "Macro %s: %s", name, out->err); program_free(out); return false; }
	snprintf(out->path, sizeof out->path, "%s", path);
	return true;
}

/* back to the operator's own program, exactly as it was */
static void macro_end(void)
{
	if (!C.macro_on) return;
	program_free(&C.prog);
	C.prog = C.saved_prog;
	memset(&C.saved_prog, 0, sizeof C.saved_prog);
	C.resume_line = C.saved_resume; C.work_done = C.saved_done; C.armed = C.saved_armed;
	C.macro_on = false;
}

/* re-fill the {placeholders} with the position and settings of this moment (Start) */
static bool macro_refresh(void)
{
	program_t p;
	if (!macro_load(C.macro_path, C.macro_name, &p)) return false;
	program_free(&C.prog);
	C.prog = p;
	return true;
}

static void finish_sim(void)
{
	if (!C.mo.dry) return;
	motion_stop(&C.mo, true);            /* nothing of the simulation may still be moving when */
	C.mo.ev = MEV_NONE;                  /* real output comes back on */
	C.mo.dry = false;
	for (int a = 0; a < NAXES; a++) motion_set_pos(&C.mo, a, sim_saved_pos[a]);
}

static void effect(cmd_t c, const arg_t *a, mstate_t from, src_t src)
{
	wcs_t *w = &WD.wcs[WD.active];
	switch (c) {
	case C_JOG:
		C.tc_confirm = false;                           /* the return move shown is out of date */
		if (a->cont) motion_jog_cont(&C.mo, a->axis, a->dir);
		else if (a->kind == 1) motion_nudge(&C.mo, a->axis, a->dir * a->dist);
		else {
			double t[NAXES], lo, hi;
			motion_queued_end(&C.mo, t);                /* chain after jogs still queued */
			t[a->axis] += a->dir * a->dist;
			if (ctl_axis_range(a->axis, &lo, &hi) && (t[a->axis] > hi || t[a->axis] < lo)) {
				t[a->axis] = CLAMP(t[a->axis], lo, hi);  /* stop AT the limit, decelerating into it */
				ctl_toast(false, "%c stops at the %s limit (%.3f)", AXIS_CH[a->axis], range_name_for(a->axis, a->dir > 0), t[a->axis]);
			}
			motion_move_to(&C.mo, t, SET.jog_feed, false);
		}
		C.touch_axis = -1;
		if (a->dist || a->cont) C.mo.last_dir[a->axis] = a->dir;
		break;
	case C_TOUCHOFF: {
		bool edge = (a->axis == AX_X || a->axis == AX_Y) && SET.edge_dia > 0;
		bool again = C.touch_axis == a->axis && C.touch_dir == a->dir;
		double target = (!edge || again) ? 0 : -a->dir * SET.edge_dia / 2;
		w->off[a->axis] = C.mo.pos[a->axis] - target;
		C.touched[a->axis] = true;
		C.touch_axis = again ? -1 : a->axis; C.touch_dir = a->dir;
		ctl_toast(false, "G%d %c = %.*f%s", 54 + WD.active, AXIS_CH[a->axis], SET.pos_dec, target,
			target ? " (edge) - same key again = 0" : "");
		workdata_save(C.wd_path);
		break;
	}
	case C_GOTOZERO: {
		double p[NAXES];
		memcpy(p, C.mo.pos, sizeof p);
		p[AX_Z] = fmax(p[AX_Z], w->off[AX_Z] + SET.safe_z);
		motion_move_to(&C.mo, p, 0, true);
		C.hold_move = false;
		snprintf(C.move_name, sizeof C.move_name, a->kind == 1 ? "Safe Z" : "Goto 0 G%d", 54 + WD.active);
		if (a->kind == 1) { ctl_toast(false, "Z up to the safe height (G%d Z%+.1f)", 54 + WD.active, SET.safe_z); break; }
		p[AX_X] = w->off[AX_X]; p[AX_Y] = w->off[AX_Y];
		motion_move_to(&C.mo, p, 0, true);
		ctl_toast(false, "G%d: Z up to safe height, then XY to 0", 54 + WD.active);
		break;
	}
	case C_MACRO: {
		/* arm: the macro becomes the program on the Run page; Start runs it, Pause holds it,
		 * Reset stops it - and the operator's own program comes back afterwards */
		macro_t m;
		if (macro_find(a->path, &m)) break;
		macro_end();
		program_t p;
		if (!macro_load(m.path, m.name, &p)) break;
		C.saved_prog = C.prog; C.prog = p;
		C.saved_resume = C.resume_line; C.saved_done = C.work_done; C.saved_armed = C.armed;
		C.resume_line = -1; C.work_done = -1; C.armed = ARM_RUN;
		snprintf(C.macro_name, sizeof C.macro_name, "%s", m.name);
		snprintf(C.macro_path, sizeof C.macro_path, "%s", m.path);
		C.macro_on = true;
		ctl_toast(false, "Macro %s armed - Start runs it, Reset cancels", m.name);
		break;
	}
	case C_HOME: C.homed = false; motion_home(&C.mo); ctl_toast(false, "Homing %s", HOME_ORDERS[SET.home_order]); break;
	case C_START:
		if (from == M_TOOL) tc_resume();
		else if (from == M_HOLD) { motion_hold(&C.mo, false); C.pause_msg[0] = 0; ctl_toast(false, C.hold_move ? "%s resumed" : "Resumed", C.move_name); }
		else start_program(src);
		C.countdown = 0;
		break;
	case C_TOOL_READY:
		snprintf(C.pause_msg, sizeof C.pause_msg, "TOOL CHANGE  ·  put in T%d", C.tc_tool);
		ctl_toast(false, "Change to T%d - jog and touch off as needed, then Start", C.tc_tool);
		break;
	case C_COOLANT:
		if (!SET.out_flood) { ctl_toast(true, "No coolant output set (Settings > Spindle & outputs > M8 output)"); break; }
		C.coolant ^= 1;
		ctl_toast(false, C.coolant & 1 ? "Coolant ON" : "Coolant OFF");
		break;
	case C_PAUSE:
		C.hold_move = from == M_MOVE;
		if (from == M_RUN) { motion_hold(&C.mo, true); if (src != SRC_MACHINE) ctl_toast(false, "Feed hold - Start resumes, Reset stops"); }
		else if (from == M_MOVE) {                      /* user 2026-10-23: a paused Goto 0 resumes with Start */
			motion_hold(&C.mo, true);
			snprintf(C.pause_msg, sizeof C.pause_msg, "%.40s on hold  ·  F1 Start continues  ·  F3 Reset stops", C.move_name);
			ctl_toast(false, "%s on hold - Start continues, Reset stops", C.move_name);
		}
		else motion_stop(&C.mo, false);
		break;
	case C_STOP:
		motion_stop(&C.mo, false);
		motion_hold(&C.mo, false);
		tc_clear();
		if (from == M_HOLD && C.hold_move) ctl_toast(false, "%s stopped", C.move_name);
		else if (from == M_RUN || from == M_HOLD || from == M_TOOL) {
			C.work_done = C.mo.line;
			ctl_toast(false, "Stopped at line %d - work done is remembered; Program > Select resumes", C.work_done + 1);
		} else if (from == M_LIMIT) ctl_toast(false, "Program cleared - limit still active, jog off the switch");
		C.feeding = false; C.countdown = 0; C.pause_msg[0] = 0; C.hold_move = false;
		all_off(); finish_sim();
		if (C.macro_on) { ctl_toast(false, "Macro %s %s", C.macro_name, from == M_IDLE ? "cancelled" : "stopped"); macro_end(); }
		break;
	case C_SPINDLE:
		C.spindle = !C.spindle;
		if (C.spindle && C.rpm <= 0) C.rpm = SET.spindle_default;
		ctl_toast(false, C.spindle ? "Spindle ON %.0f RPM" : "Spindle OFF", C.rpm);
		break;
	case C_DRIVES_ON: C.stop_cause[0] = 0; ctl_toast(false, "Drives ON"); break;
	case C_DRIVES_OFF:
		motion_stop(&C.mo, true); all_off();
		snprintf(C.stop_cause, sizeof C.stop_cause, "%s", src == SRC_API ? "switched off over the network (API)" : "switched off with Drives (F3) on the Jog screen");
		ctl_toast(false, "Drives OFF");
		break;
	case C_LOAD: {
		macro_end();
		int r = a->text ? program_load_text(&C.prog, a->path ? a->path : "upload.nc", a->text, a->len)
		                : program_load(&C.prog, a->path);
		C.resume_line = -1; C.work_done = -1;
		if (r) ctl_toast(true, "Load failed: %s", C.prog.err);
		else ctl_toast(false, "Loaded %s - %d lines%s%s", C.prog.name, C.prog.nlines, C.prog.warn[0] ? " - " : "", C.prog.warn);
		break;
	}
	case C_OVERRIDE:
		/* up to 300 % (user: rescues a program with odd F/S values); rapid stays min(feed, 100) */
		if (a->kind == 0) C.mo.feed_ov = CLAMP(C.mo.feed_ov + a->delta, 0, 300);
		else C.mo.spin_ov = CLAMP(C.mo.spin_ov + a->delta, 0, 300);
		break;
	case C_ESTOP_IN:
		motion_stop(&C.mo, !SET.graceful_stop); C.feeding = false; all_off(); finish_sim(); tc_clear();
		if (from == M_RUN || from == M_TOOL || (from == M_HOLD && !C.hold_move)) C.work_done = C.mo.line;
		motion_hold(&C.mo, false); C.hold_move = false;
		macro_end();
		ctl_toast(true, "E-STOP: %s", C.stop_cause[0] ? C.stop_cause : "E-stop input");
		break;
	case C_ESTOP_OUT: {
		char c[96];
		snprintf(c, sizeof c, "%s", C.stop_cause[0] ? C.stop_cause : "an E-stop input");
		snprintf(C.stop_cause, sizeof C.stop_cause, "stopped by %.60s (now released)", c);
		ctl_toast(false, "E-stop released - ready (%s)", c);
		break;
	}
	case C_LIMIT_HIT:
		motion_stop(&C.mo, !SET.graceful_stop);   /* brakes past the switch: the overshoot is v^2/2a */
		if (from == M_RUN || from == M_TOOL) C.work_done = C.mo.line;
		C.feeding = false; all_off(); finish_sim(); tc_clear();
		macro_end();
		C.limit_axis = a->axis; C.limit_dir = a->dir;
		C.lim_dir[a->axis] = a->dir ? a->dir : 2;
		{
			char also[48] = "";
			for (int k = 0; k < NAXES; k++)
				if (k != a->axis && C.lim_dir[k]) {
					size_t l = strlen(also);
					snprintf(also + l, sizeof also - l, "%s%c%s", l ? ", " : " (also ", AXIS_CH[k], C.lim_dir[k] == 1 ? "+" : C.lim_dir[k] == -1 ? "-" : "");
				}
			if (also[0]) strncat(also, ")", sizeof also - strlen(also) - 1);
			ctl_toast(true, "%c%s limit%s - only those directions are blocked", AXIS_CH[a->axis], a->dir > 0 ? "+" : a->dir < 0 ? "-" : "", also);
		}
		break;
	case C_LIMIT_CLEAR: ctl_toast(false, "Off the switches - limits cleared"); C.limit_dir = 0; C.limit_axis = -1; break;
	case C_DONE:
		if (from == M_HOMING) {
			C.homed = a->dir == 1;
			ctl_toast(!C.homed, C.homed ? "Homed - soft limits active" : "Homing failed: switch not found within travel");
		} else if (from == M_RUN) {
			C.work_done = C.prog.nlines - 1; all_off(); finish_sim(); tc_clear();
			if (C.macro_on) { ctl_toast(false, "Macro %s done", C.macro_name); macro_end(); }
			else ctl_toast(false, "Program finished");
		} else if (from == M_MOVE) { ctl_toast(false, "%s done", C.move_name[0] ? C.move_name : "Move"); C.move_name[0] = 0; }
		break;
	default: break;
	}
}

bool act(cmd_t c, const arg_t *a, src_t src)
{
	static const arg_t none;
	if (!a) a = &none;
	if (c == C_START && C.m == M_IDLE && C.macro_on && !macro_refresh()) return false;
	const char *why = can(c, a, src);
	if (why) { if (src != SRC_MACHINE) ctl_toast(true, "%s", why); return false; }
	if (c == C_START && C.m == M_TOOL && !C.tc_confirm) {   /* user: Start asks first, then goes */
		double t[NAXES], via;
		double v = ctl_tc_target(t, &via);
		const double *o = WD.wcs[ctl_prog_wcs()].off;
		C.tc_confirm = true;
		ctl_toast(false, "Start again: straight to X%.3f Y%.3f Z%.3f at %.0f mm/min, then T%d continues",
			t[AX_X] - o[AX_X], t[AX_Y] - o[AX_Y], via - o[AX_Z], v, C.tc_tool);
		return true;
	}
	mstate_t from = C.m;
	effect(c, a, from, src);
	C.m = next_state(c, from);
	return true;
}
