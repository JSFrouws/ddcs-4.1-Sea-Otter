#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "estimate.h"
#include "machine.h"
#include "motion.h"
#include "settings.h"

static const double LV[EST_NLV] = { 1.0, 2.0 };

/* Segments are planned in windows: like the real queue, a window ends in a stop. When it
 * fills up, all but its last KEEP segments are finished and the rest carried over, so a
 * long chain of moves is never cut into stops (only a window this long, rarely, would).
 * Speeds are kept SQUARED in the passes: the unit has no FPU and a square root is the most
 * expensive step - one per segment and level is left (for its time). */
#define WIN 192                     /* small: one window's planning must fit in a tick's budget (~1 ms on the unit) */
#define KEEP 64

typedef struct { struct plan p; int line; double vj2, vr2[EST_NLV], vb2[EST_NLV], ve2[EST_NLV]; } seg_t;

typedef struct {
	int nlines;
	/* the result, filled as the time is booked (lines come in order): cum[i * EST_NLV + k] =
	 * seconds before line i at level k - so finishing costs nothing (a prefix sum over 64 000
	 * lines at the end took 40-50 ms in one tick on the unit) */
	float *cum; int filled[EST_NLV]; double run[EST_NLV];
	seg_t *w; int n;
	double pos[NAXES];
	bool prev_line;                   /* the move before this one was a line (no stop in between) */
	struct plan prev;                 /* ... that line (corner limit) */
	double prev_vr2[EST_NLV], last_ve2[EST_NLV];   /* its speed and its entry speed, squared */
} est_t;

static void book(est_t *e, int k, int line, double t)
{
	while (e->filled[k] < line) e->cum[(size_t)++e->filled[k] * EST_NLV + k] = (float)e->run[k];
	e->run[k] += t;
}

/* the planner's speed for a segment at override level k: rapids at most 100 % */
static double vrun(const struct plan *p, int k) { return fmin(p->vnom * (p->rapid ? 1.0 : LV[k]), p->vcap); }

/* trapezoid: from v0 up to vc, cruise, down to v1, over L at acceleration a */
static double seg_time(double v0, double v1, double vc, double a, double L)
{
	if (vc < 1e-9 || a < 1e-9) return 0;
	v0 = fmin(v0, vc); v1 = fmin(v1, vc);
	double da = (vc * vc - v0 * v0) / (2 * a), dd = (vc * vc - v1 * v1) / (2 * a);
	if (da + dd <= L) return (2 * vc - v0 - v1) / a + (L - da - dd) / vc;
	double vp = sqrt((2 * a * L + v0 * v0 + v1 * v1) / 2);
	vp = fmax(vp, fmax(v0, v1));
	return (2 * vp - v0 - v1) / a;
}

/* plan the window (a stop after its last segment) and book the time of the finished part */
static void flush(est_t *e, bool stop)
{
	int n = e->n;
	if (!n) return;
	int done = stop ? n : n - KEEP;
	for (int k = 0; k < EST_NLV; k++) {
		/* backward: corner, own and previous speed, and braking for everything after */
		double next2 = 0;
		for (int i = n - 1; i >= 0; i--) {
			seg_t *s = &e->w[i];
			double lim2 = fmin(s->vj2, s->vr2[k]);
			if (i) lim2 = fmin(lim2, e->w[i - 1].vr2[k]);
			else lim2 = e->prev_line ? fmin(lim2, e->prev_vr2[k]) : 0;
			s->vb2[k] = fmin(lim2, next2 + 2 * s->p.aplan * s->p.len);
			next2 = s->vb2[k];
		}
		/* forward: what acceleration can reach from the speed before */
		for (int i = 0; i < n; i++) {
			seg_t *s = &e->w[i];
			double v2 = s->vb2[k];
			if (i) v2 = fmin(v2, e->w[i - 1].ve2[k] + 2 * e->w[i - 1].p.aplan * e->w[i - 1].p.len);
			else v2 = e->prev_line ? fmin(v2, e->last_ve2[k] + 2 * e->prev.aplan * e->prev.len) : 0;
			s->ve2[k] = v2;
		}
		double v0 = sqrt(e->w[0].ve2[k]);
		for (int i = 0; i < done; i++) {
			const seg_t *s = &e->w[i];
			double v1 = i + 1 < n ? sqrt(e->w[i + 1].ve2[k]) : 0;
			book(e, k, s->line, seg_time(v0, v1, sqrt(s->vr2[k]), s->p.aplan, s->p.len));
			v0 = v1;
		}
	}
	if (stop) { e->n = 0; e->prev_line = false; return; }
	e->prev = e->w[done - 1].p;
	for (int k = 0; k < EST_NLV; k++) { e->last_ve2[k] = e->w[done - 1].ve2[k]; e->prev_vr2[k] = e->w[done - 1].vr2[k]; }
	e->prev_line = true;
	memmove(e->w, e->w + done, sizeof(seg_t) * (size_t)(n - done));
	e->n = n - done;
}

static void est_free(est_t *e)
{
	if (!e) return;
	free(e->cum); free(e->w); free(e);
}

static est_t *est_begin(int nlines, const double start[NAXES])
{
	est_t *e = calloc(1, sizeof *e);
	if (!e) return NULL;
	e->nlines = nlines;
	e->w = malloc(sizeof(seg_t) * WIN);
	e->cum = calloc(((size_t)nlines + 1) * EST_NLV, sizeof(float));
	if (!e->w || !e->cum) { est_free(e); return NULL; }
	memcpy(e->pos, start, sizeof e->pos);
	return e;
}

static void est_op(est_t *e, const gc_op_t *op)
{
	if (op->kind == OP_END) return;
	int line = op->line >= 0 && op->line < e->nlines ? op->line : e->nlines;
	if (op->kind == OP_LINE) {
		seg_t s = { .line = line };
		if (!motion_plan_setup(&s.p, op, e->pos)) return;   /* zero length */
		memcpy(e->pos, op->end, sizeof e->pos);
		const struct plan *pv = e->n ? &e->w[e->n - 1].p : e->prev_line ? &e->prev : NULL;
		double vj = pv ? motion_corner_limit(pv, &s.p) : 0;
		s.vj2 = vj * vj;
		for (int k = 0; k < EST_NLV; k++) { double v = vrun(&s.p, k); s.vr2[k] = v * v; }
		e->w[e->n++] = s;
		if (e->n == WIN) flush(e, false);
		return;
	}
	/* anything else stops the motion first (the planner treats it so), then takes its time */
	flush(e, true);
	double t = 0;
	if (op->kind == OP_DWELL) t = op->value;
	if (op->kind == OP_SPINDLE && op->ival && SET.spindle_delay > 0) t = SET.spindle_delay;
	if (t > 0) for (int k = 0; k < EST_NLV; k++) book(e, k, line, t);
}

static void est_end(est_t *e, program_t *p)
{
	flush(e, true);
	int n = e->nlines;
	for (int k = 0; k < EST_NLV; k++) {                    /* the lines after the last move */
		while (e->filled[k] < n) e->cum[(size_t)++e->filled[k] * EST_NLV + k] = (float)e->run[k];
		e->cum[(size_t)n * EST_NLV + k] = (float)e->run[k];
	}
	free(p->est_cum);
	p->est_cum = e->cum; e->cum = NULL;                    /* handed over: visible from now on */
	p->est_s = p->est_cum[(size_t)n * EST_NLV] * EST_MARGIN + EST_MARGIN_S;
	est_free(e);
}

/* ---- the background job: the program's ops again, a few ms per tick ---- */
static struct { bool on; uint32_t gen; gc_t g; est_t *e; } J;

static void job_stop(void) { est_free(J.e); J.e = NULL; J.on = false; }

static bool job_start(program_t *p)
{
	job_stop();
	/* the same start as the load's dry run (program_load_text): work zero, Z at safe height */
	const double *off = WD.wcs[WD.active].off;
	double start[NAXES];
	for (int a = 0; a < NAXES; a++) start[a] = off[a];
	start[AX_Z] = off[AX_Z] + SET.safe_z;
	gc_begin(&J.g, p->text, p->len, start);
	J.e = est_begin(p->nlines, start);
	J.gen = p->gen; J.on = J.e != NULL;
	return J.on;
}

/* true = finished; budget_ms < 0: no limit */
static bool job_run(program_t *p, int budget_ms)
{
	uint64_t t0 = now_ms();
	gc_op_t op;
	int r, n = 0;
	while ((r = gc_next(&J.g, &op)) > 0) {
		est_op(J.e, &op);
		if (budget_ms >= 0 && (++n & 7) == 0 && now_ms() - t0 >= (uint64_t)budget_ms) return false;
	}
	est_end(J.e, p);                                        /* also after an error: up to there */
	J.e = NULL; J.on = false;
	return true;
}

bool est_background(int budget_ms)
{
	program_t *p = &C.prog;
	if (!p->loaded || !p->text || p->est_cum) { if (J.on && (!p->loaded || J.gen != p->gen)) job_stop(); return true; }
	if (!J.on || J.gen != p->gen) if (!job_start(p)) return true;
	return job_run(p, budget_ms);
}

void est_compute(program_t *p)
{
	if (!p->loaded || !p->text) return;
	if (job_start(p)) job_run(p, -1);
}

double est_span(const program_t *p, int a, int b, int ov)
{
	if (!p->est_cum || ov <= 0) return -1;
	int n = p->nlines;
	a = a < 0 ? 0 : a > n ? n : a;
	b = b < a ? a : b > n ? n : b;
	double T[EST_NLV];
	for (int k = 0; k < EST_NLV; k++) T[k] = (double)p->est_cum[(size_t)b * EST_NLV + k] - p->est_cum[(size_t)a * EST_NLV + k];
	double f = ov / 100.0;
	if (f <= LV[0]) return T[0] / f;                     /* everything (rapids too) slower */
	if (f >= LV[EST_NLV - 1]) return T[EST_NLV - 1];      /* above the top level: that level's time (over) */
	int i = 0;
	while (f > LV[i + 1]) i++;
	double x = 1 / f, x0 = 1 / LV[i], x1 = 1 / LV[i + 1];
	double w = (x - x1) / (x0 - x1);
	return w * T[i] + (1 - w) * T[i + 1];
}

/* one straight move from rest to rest at override ov %, as the planner would run it */
static double move_time(const double a[NAXES], const double b[NAXES], bool rapid, double feed, int ov)
{
	gc_op_t op = { .kind = OP_LINE, .rapid = rapid, .feed = feed };
	memcpy(op.end, b, sizeof op.end);
	struct plan p;
	if (!motion_plan_setup(&p, &op, a)) return 0;
	double f = ov / 100.0;
	return seg_time(0, 0, fmin(p.vnom * (rapid ? fmin(f, 1) : f), p.vcap), p.aplan, p.len);
}

/* idle: the estimate starts at work zero / safe Z, the run from where the machine is */
static double start_correction(const program_t *p, int ov)
{
	if (!p->has_first) return 0;
	const double *off = WD.wcs[WD.active].off;
	double s[NAXES], t[NAXES];
	for (int a = 0; a < NAXES; a++) { s[a] = off[a]; t[a] = p->first_end[a] + off[a]; }
	s[AX_Z] = off[AX_Z] + SET.safe_z;
	return move_time(C.mo.pos, t, p->first_rapid, p->first_feed, ov) - move_time(s, t, p->first_rapid, p->first_feed, ov);
}

/* running: the line in progress takes the longer of its estimate and what the move in
 * progress still has to go (an approach from far away is one long move of that line) */
static double current_line(const program_t *p, int line, int ov)
{
	double est = est_span(p, line, line + 1, ov), left = 0;
	const motion_t *m = &C.mo;
	if (m->active && m->op.kind == OP_LINE) {
		double f = ov / 100.0, v = fmin(m->cur.vnom * (m->cur.rapid ? fmin(f, 1) : f), m->cur.vcap);
		if (v > 1e-6) left = fmax(m->len - m->s, 0) / v + v / fmax(m->cur.aplan, 1e-6);
	} else if (m->active && m->op.kind == OP_DWELL) left = m->dwell_left;
	return fmax(est, left);
}

double est_remaining(const program_t *p, int from, int ov)
{
	double t = est_span(p, from, p->nlines, ov);
	return t < 0 ? -1 : t * EST_MARGIN + EST_MARGIN_S;
}

/* a multi-part job cuts each tool segment (core/ctl_tick.c) on every JOB part in turn */
static int seg_first(const program_t *p, int s) { return s > 0 ? p->tc_line[s - 1] : 0; }
static int seg_end(const program_t *p, int s) { return s < p->ntc ? p->tc_line[s] : p->nlines; }

double est_now(bool *left)
{
	const program_t *p = &C.prog;
	int ov = C.mo.feed_ov;
	bool run = C.m == M_RUN || C.m == M_HOLD || C.m == M_TOOL;
	*left = run;
	if (!p->loaded || ov <= 0) return -1;
	if (!p->est_cum) return -2;
	int parts = 0;
	for (int i = 0; i < NWCS; i++) parts += WD.wcs[i].job;
	int line = run && C.mo.line > 0 ? C.mo.line : 0;
	double first = run ? current_line(p, line, ov) - est_span(p, line, line + 1, ov) : start_correction(p, ov);
	if (parts < 2 || (run && C.job_idx < 0)) return fmax(est_span(p, line, p->nlines, ov) + first, 0) * EST_MARGIN + EST_MARGIN_S;
	int nseg = p->ntc + 1, seg = run ? CLAMP(C.job_seg, 0, nseg - 1) : 0;
	double t = 0;
	if (run) {
		int after = 0;                                       /* parts still to come for this tool */
		for (int i = C.job_idx + 1; i < NWCS; i++) after += WD.wcs[i].job;
		t += est_span(p, CLAMP(line, seg_first(p, seg), seg_end(p, seg)), seg_end(p, seg), ov);
		t += est_span(p, seg_first(p, seg), seg_end(p, seg), ov) * after;
		seg++;
	}
	for (int s = seg; s < nseg; s++) t += est_span(p, seg_first(p, s), seg_end(p, s), ov) * parts;
	return fmax(t + first, 0) * EST_MARGIN + EST_MARGIN_S;
}
