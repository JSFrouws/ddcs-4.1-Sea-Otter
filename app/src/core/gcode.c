#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gcode.h"
#include "settings.h"

#define TOL 0.005

void gc_begin(gc_t *g, const char *text, size_t len, const double mpos[NAXES])
{
	memset(g, 0, sizeof *g);
	g->text = text; g->len = len;
	g->absolute = true; g->plane = 17; g->wcs = WD.active; g->motion = 0;
	g->retract_r = false;
	g->default_feed = SET.default_feed; g->feed = SET.default_feed;
	memcpy(g->pos, mpos, sizeof g->pos);
	g->line = -1;
	g->force_wcs = -1;
	for (int i = 0; i < NWCS; i++) memcpy(g->wcs_off[i], WD.wcs[i].off, sizeof g->wcs_off[i]);
}

typedef struct {
	double v[26]; bool has[26];
	int g[12], ng, m[8], nm;
} words_t;

static int read_line(gc_t *g, char *buf, size_t n)
{
	if (g->cur >= g->len) return 0;
	size_t i = 0;
	while (g->cur < g->len && g->text[g->cur] != '\n') {
		if (i + 1 < n) buf[i++] = g->text[g->cur];
		g->cur++;
	}
	if (g->cur < g->len) g->cur++;
	buf[i] = 0;
	g->line++;
	return 1;
}

static int parse(gc_t *g, char *s, words_t *w)
{
	memset(w, 0, sizeof *w);
	int depth = 0;
	for (char *p = s; *p; p++) {                 /* blank out comments */
		if (*p == ';') { *p = 0; break; }
		if (*p == '(') depth++;
		if (depth) { if (*p == ')') depth--; *p = ' '; }
	}
	char *p = s;
	while (*p) {
		while (*p && (isspace((unsigned char)*p) || *p == '%')) p++;
		if (!*p) break;
		int L = toupper((unsigned char)*p++);
		if (L < 'A' || L > 'Z') { snprintf(g->err, sizeof g->err, "unexpected '%c'", L); return -1; }
		char *e;
		double v = strtod(p, &e);
		if (e == p) { snprintf(g->err, sizeof g->err, "%c without a number", L); return -1; }
		p = e;
		if (L == 'G') { if (w->ng < 12) w->g[w->ng++] = (int)lround(v * 10); }
		else if (L == 'M') { if (w->nm < 8) w->m[w->nm++] = (int)lround(v); }
		else { w->v[L - 'A'] = v; w->has[L - 'A'] = true; }
	}
	return 0;
}

static bool hasg(const words_t *w, int code10) { for (int i = 0; i < w->ng; i++) if (w->g[i] == code10) return true; return false; }
static bool hasm(const words_t *w, int code) { for (int i = 0; i < w->nm; i++) if (w->m[i] == code) return true; return false; }

static gc_op_t *push(gc_t *g, op_kind_t k)
{
	gc_op_t *o = &g->q[g->qn++];
	memset(o, 0, sizeof *o);
	o->kind = k; o->line = g->line; o->feed = g->feed;
	memcpy(o->end, g->pos, sizeof o->end);
	return o;
}

static void line_to(gc_t *g, const double t[NAXES], bool rapid)
{
	gc_op_t *o = push(g, OP_LINE);
	o->rapid = rapid;
	memcpy(o->end, t, sizeof o->end);
	memcpy(g->pos, t, sizeof g->pos);
}

static const char AXL[NAXES] = { 'X', 'Y', 'Z', 'A' };

/* target in machine coordinates for this block */
static void target(gc_t *g, const words_t *w, bool g53, double t[NAXES])
{
	double k = g->inches ? 25.4 : 1;
	for (int a = 0; a < NAXES; a++) {
		t[a] = g->pos[a];
		if (!w->has[AXL[a] - 'A']) continue;
		double v = w->v[AXL[a] - 'A'] * (a == AX_A ? 1 : k);
		if (g53) t[a] = v;
		else if (g->absolute) t[a] = v + g->wcs_off[g->force_wcs >= 0 ? g->force_wcs : g->wcs][a];
		else t[a] = g->pos[a] + v;
	}
}

static int start_arc(gc_t *g, const words_t *w, const double t[NAXES], bool cw)
{
	int a0 = g->plane == 17 ? AX_X : g->plane == 18 ? AX_Z : AX_Y;
	int a1 = g->plane == 17 ? AX_Y : g->plane == 18 ? AX_X : AX_Z;
	int a2 = g->plane == 17 ? AX_Z : g->plane == 18 ? AX_Y : AX_X;
	double k = g->inches ? 25.4 : 1;
	double s0 = g->pos[a0], s1 = g->pos[a1], e0 = t[a0], e1 = t[a1], c0, c1;
	if (w->has['R' - 'A']) {
		double r = w->v['R' - 'A'] * k, dx = e0 - s0, dy = e1 - s1, d = hypot(dx, dy);
		if (d < 1e-9 || fabs(r) * 2 < d - 1e-6) { snprintf(g->err, sizeof g->err, "arc radius too small"); return -1; }
		double h = sqrt(fmax(0, r * r - d * d / 4)) * ((cw ^ (r < 0)) ? -1 : 1);
		c0 = s0 + dx / 2 - h * dy / d; c1 = s1 + dy / 2 + h * dx / d;
	} else {
		const char ci = g->plane == 17 ? 'I' : g->plane == 18 ? 'K' : 'J';
		const char cj = g->plane == 17 ? 'J' : g->plane == 18 ? 'I' : 'K';
		c0 = s0 + w->v[ci - 'A'] * k; c1 = s1 + w->v[cj - 'A'] * k;
	}
	double r = hypot(s0 - c0, s1 - c1);
	if (r < 1e-6) { snprintf(g->err, sizeof g->err, "arc with zero radius"); return -1; }
	if (fabs(hypot(e0 - c0, e1 - c1) - r) > 0.05 + r * 1e-3) { snprintf(g->err, sizeof g->err, "arc end not on the circle"); return -1; }
	double st = atan2(s1 - c1, s0 - c0), en = atan2(e1 - c1, e0 - c0), sw = en - st;
	if (cw && sw >= -1e-9) sw -= 2 * M_PI;
	if (!cw && sw <= 1e-9) sw += 2 * M_PI;
	double step = 2 * acos(fmax(-1.0, 1 - TOL / r));
	int n = (int)ceil(fabs(sw) / fmax(step, 1e-3));
	g->arc.on = true; g->arc.n = CLAMP(n, 1, 4000); g->arc.i = 0;
	g->arc.a0 = a0; g->arc.a1 = a1; g->arc.a2 = a2;
	g->arc.c0 = c0; g->arc.c1 = c1; g->arc.r = r; g->arc.start = st; g->arc.sweep = sw;
	g->arc.h0 = g->pos[a2]; g->arc.dh = t[a2] - g->pos[a2];
	memcpy(g->arc.end, t, sizeof g->arc.end);
	g->arc.line = g->line;
	g->arc_seq++;
	return 0;
}

static void arc_chord(gc_t *g)
{
	g->arc.i++;
	double t[NAXES];
	memcpy(t, g->arc.end, sizeof t);
	if (g->arc.i < g->arc.n) {
		double f = (double)g->arc.i / g->arc.n, a = g->arc.start + g->arc.sweep * f;
		t[g->arc.a0] = g->arc.c0 + g->arc.r * cos(a);
		t[g->arc.a1] = g->arc.c1 + g->arc.r * sin(a);
		t[g->arc.a2] = g->arc.h0 + g->arc.dh * f;
	} else g->arc.on = false;
	gc_op_t *o = push(g, OP_LINE);
	o->line = g->arc.line; o->arc = g->arc_seq;
	memcpy(o->end, t, sizeof o->end);
	memcpy(g->pos, t, sizeof g->pos);
}

/* Canned cycles. G81 drill, G82 + dwell, G83 peck (back out to R after each peck), G73 chip
 * break (back off PECK_CLEAR), G85 bore (feed out), G89 bore + dwell. Each hole: rapid up to R
 * if below it, rapid XY, rapid to R, cut, then rapid to the return level (G98: the Z the cycle
 * started at, G99: R). */
#define PECK_CLEAR 0.25          /* mm: G73 back-off, and G83's rapid re-approach stops this short */

static void hole_start(gc_t *g, const double t[NAXES])
{
	double *p = g->hole.p;
	memcpy(p, g->pos, sizeof g->hole.p);
	if (p[AX_Z] < g->cy_r) { p[AX_Z] = g->cy_r; line_to(g, p, true); }
	p[AX_X] = t[AX_X]; p[AX_Y] = t[AX_Y];
	line_to(g, p, true);
	p[AX_Z] = g->cy_r; line_to(g, p, true);
	g->hole.on = true; g->hole.phase = 1; g->hole.n = 0; g->hole.depth = g->cy_r;
	g->hole.ret = g->retract_r ? g->cy_r : fmax(g->cy_init, g->cy_r);
}

/* the next few ops of the hole in progress (at most 3, so the op queue never overflows) */
static void hole_step(gc_t *g)
{
	double *p = g->hole.p, r = g->cy_r, zb = g->cy_z;
	int c = g->cycle;
	if (g->hole.phase == 1) {                                 /* cut down to the next depth */
		bool peck = (c == 73 || c == 83) && g->cy_q > 0;
		double next = peck ? r - ++g->hole.n * g->cy_q : zb;    /* counted, so fine pecks don't drift */
		if (next < zb + 1e-6) next = zb;
		if (c == 83 && g->hole.depth < r - 1e-9) { p[AX_Z] = fmin(r, g->hole.depth + PECK_CLEAR); line_to(g, p, true); }
		p[AX_Z] = next; line_to(g, p, false);
		g->hole.depth = next;
		g->hole.phase = next > zb + 1e-9 ? 3 : 2;
	} else if (g->hole.phase == 3) {                          /* between pecks: clear the chips */
		p[AX_Z] = c == 83 ? r : fmin(r, g->hole.depth + PECK_CLEAR);
		line_to(g, p, true);
		g->hole.phase = 1;
	} else {                                                  /* bottom: dwell, back out */
		if ((c == 82 || c == 89) && g->cy_p > 0) push(g, OP_DWELL)->value = g->cy_p;
		if (c == 85 || c == 89) { p[AX_Z] = r; line_to(g, p, false); }
		if (p[AX_Z] != g->hole.ret) { p[AX_Z] = g->hole.ret; line_to(g, p, true); }
		g->hole.on = false;
	}
}

static int exec_block(gc_t *g, words_t *w, bool emit)
{
	double k = g->inches ? 25.4 : 1;
	if (w->has['F' - 'A']) g->feed = w->v['F' - 'A'] * k;
	if (w->has['S' - 'A']) g->rpm = w->v['S' - 'A'];
	if (w->has['T' - 'A']) g->tool = (int)w->v['T' - 'A'];
	if (hasm(w, 6) && emit) { gc_op_t *o = push(g, OP_PAUSE); o->ival = PAUSE_TOOL; o->value = g->tool; }
	int sp = hasm(w, 3) ? 3 : hasm(w, 4) ? 4 : hasm(w, 5) ? 0 : -1;
	if (sp >= 0 || (w->has['S' - 'A'] && g->spindle)) {
		if (sp >= 0) g->spindle = sp;
		if (emit) { gc_op_t *o = push(g, OP_SPINDLE); o->ival = g->spindle; o->value = g->rpm; }
	}
	int cm = g->coolant;
	if (hasm(w, 8)) cm |= 1;
	if (hasm(w, 7)) cm |= 2;
	if (hasm(w, 9)) cm &= 4;
	if (hasm(w, 10)) cm |= 4;
	if (hasm(w, 11)) cm &= ~4;
	if (cm != g->coolant) { g->coolant = cm; if (emit) push(g, OP_COOLANT)->ival = cm; }
	int prev_cycle = g->cycle;
	if (hasg(w, 40)) { if (emit) { gc_op_t *o = push(g, OP_DWELL); o->value = w->v['P' - 'A']; } }
	for (int i = 0; i < w->ng; i++) {
		int c = w->g[i];
		if (c == 170 || c == 180 || c == 190) g->plane = c / 10;
		else if (c == 200) g->inches = true;
		else if (c == 210) g->inches = false;
		else if (c == 900) g->absolute = true;
		else if (c == 910) g->absolute = false;
		else if (c >= 540 && c <= 590 && c % 10 == 0) g->wcs = c / 10 - 54;
		else if (c == 980) g->retract_r = false;
		else if (c == 990) g->retract_r = true;
		else if (c == 0 || c == 10 || c == 20 || c == 30) { g->motion = c / 10; g->cycle = 0; }
		else if (c == 730 || c == 810 || c == 820 || c == 830 || c == 850 || c == 890) g->cycle = c / 10;
		else if (c == 800) g->cycle = 0;                    /* back to the G0/G1 mode from before */
		else if (c == 400 || c == 430 || c == 490 || c == 610 || c == 640 || c == 940 || c == 530 || c == 280 || c == 40 || c == 100) {
			if (c == 430 && !g->warn[0]) { snprintf(g->warn, sizeof g->warn, "G43 ignored (no length comp)"); g->warn_line = g->line; }
		} else if (c == 410 || c == 420) { snprintf(g->err, sizeof g->err, "cutter compensation (G41/G42) not supported"); return -1; }
		else { snprintf(g->err, sizeof g->err, "G%g not supported", c / 10.0); return -1; }
	}
	if (hasg(w, 100)) {                                      /* G10 L2 / L20 Pn */
		int p = (int)w->v['P' - 'A'], l = (int)w->v['L' - 'A'];
		int idx = p ? p - 1 : g->wcs;
		if (idx < 0 || idx >= NWCS) { snprintf(g->err, sizeof g->err, "G10 P out of range"); return -1; }
		for (int a = 0; a < NAXES; a++) {
			if (!w->has[AXL[a] - 'A']) continue;
			double v = w->v[AXL[a] - 'A'] * (a == AX_A ? 1 : k);
			g->wcs_off[idx][a] = l == 20 ? g->pos[a] - v : v;
			if (g->apply_g10) WD.wcs[idx].off[a] = g->wcs_off[idx][a];
		}
		return 0;
	}
	bool g53 = hasg(w, 530);
	bool any_axis = false;
	for (int a = 0; a < NAXES; a++) any_axis |= w->has[AXL[a] - 'A'];
	double t[NAXES];
	if (hasg(w, 280)) {                                      /* G28: named axes go home */
		memcpy(t, g->pos, sizeof t);
		for (int a = 0; a < NAXES; a++) if (w->has[AXL[a] - 'A'] || !any_axis) t[a] = SET.ax[a].home_pos;
		if (emit) line_to(g, t, true); else memcpy(g->pos, t, sizeof t);
		return 0;
	}
	if (g->cycle) {
		double off = g->wcs_off[g->force_wcs >= 0 ? g->force_wcs : g->wcs][AX_Z];
		if (!prev_cycle) { g->cy_init = g->cy_r = g->cy_z = g->pos[AX_Z]; g->cy_q = g->cy_p = 0; }
		/* R, Z, Q, P stay in force for the following holes; in G91, R is from the start level
		 * and Z from R */
		if (w->has['R' - 'A']) g->cy_r = w->v['R' - 'A'] * k + (g->absolute ? off : g->cy_init);
		if (w->has['Z' - 'A']) g->cy_z = w->v['Z' - 'A'] * k + (g->absolute ? off : g->cy_r);
		if (w->has['Q' - 'A']) g->cy_q = fabs(w->v['Q' - 'A'] * k);
		if (w->has['P' - 'A']) g->cy_p = w->v['P' - 'A'];
		if (!any_axis) return 0;
		if (g->cy_z >= g->cy_r - 1e-9) { snprintf(g->err, sizeof g->err, "G%d: hole bottom Z not below R", g->cycle); return -1; }
		target(g, w, false, t);                                 /* only its X/Y are used */
		if (!emit) {
			g->pos[AX_X] = t[AX_X]; g->pos[AX_Y] = t[AX_Y];
			g->pos[AX_Z] = g->retract_r ? g->cy_r : fmax(g->cy_init, g->cy_r);
			return 0;
		}
		hole_start(g, t);
		return 0;
	}
	if (!any_axis) return 0;
	target(g, w, g53, t);
	if (!emit) {                       /* seek: just track position */
		memcpy(g->pos, t, sizeof t);
		return 0;
	}
	if (g53 || g->motion == 0) line_to(g, t, true);
	else if (g->motion == 1) line_to(g, t, false);
	else if (g->motion == 2 || g->motion == 3) return start_arc(g, w, t, g->motion == 2);
	return 0;
}

static int next_block(gc_t *g, bool emit)
{
	char buf[256];
	words_t w;
	if (!read_line(g, buf, sizeof buf)) return 0;
	if (parse(g, buf, &w) < 0 || exec_block(g, &w, emit) < 0) { g->err_line = g->line; return -1; }
	if (emit) {
		if (hasm(&w, 0)) push(g, OP_PAUSE)->ival = PAUSE_M0;
		if (hasm(&w, 1)) push(g, OP_PAUSE)->ival = PAUSE_M1;
		if (hasm(&w, 2) || hasm(&w, 30)) { push(g, OP_END); g->cur = g->len; }
	}
	return 1;
}

int gc_next(gc_t *g, gc_op_t *op)
{
	for (;;) {
		if (g->qi < g->qn) { *op = g->q[g->qi++]; return 1; }
		g->qi = g->qn = 0;
		if (g->arc.on) { arc_chord(g); continue; }
		if (g->hole.on) { hole_step(g); continue; }
		int r = next_block(g, true);
		if (r <= 0) return r;
	}
}

int gc_seek(gc_t *g, int line)
{
	while (g->line + 1 < line) {
		size_t before = g->cur;
		int r = next_block(g, false);
		if (r < 0) return -1;
		if (r == 0 || g->cur == before) break;
	}
	return 0;
}
