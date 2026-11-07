#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "gcode.h"
#include "settings.h"

#define TOL 0.005
#define VACANT NAN

static const char AXL[NAXES] = { 'X', 'Y', 'Z', 'A' };
#define W(c) ((c) - 'A')

void gc_begin(gc_t *g, const char *text, size_t len, const double mpos[NAXES])
{
	memset(g, 0, sizeof *g);
	g->text = text; g->len = len;
	g->absolute = true; g->plane = 17; g->wcs = WD.active; g->motion = 0;
	g->retract_r = false;
	g->default_feed = SET.default_feed; g->feed = SET.default_feed;
	g->feedmode = 94; g->ccomp = 40; g->tlo_code = 49;
	memcpy(g->pos, mpos, sizeof g->pos);
	g->line = -1;
	g->force_wcs = -1;
	g->prog_no = -1; g->seq_no = -1;
	for (int i = 0; i < NWCS; i++) memcpy(g->wcs_off[i], WD.wcs[i].off, sizeof g->wcs_off[i]);
	for (int i = 0; i < NTOOLS; i++) { g->tl_len[i] = WD.tool[i].len; g->tl_dia[i] = WD.tool[i].dia; }
	for (int l = 0; l <= GC_DEPTH; l++) for (int i = 0; i < 33; i++) g->loc[l][i] = VACANT;
	for (int i = 0; i < 100; i++) g->com[i] = VACANT;
	for (int i = 0; i < 500; i++) g->keep[i] = VACANT;
}

/* ---- messages ---- */

static int fail(gc_t *g, const char *fmt, ...)
{
	if (!g->bad) {
		va_list ap; va_start(ap, fmt);
		vsnprintf(g->err, sizeof g->err, fmt, ap);
		va_end(ap);
	}
	g->bad = true;
	return -1;
}

/* every distinct warning once: "first; line 9: second", then a count */
static void warnf(gc_t *g, const char *fmt, ...)
{
	char m[96];
	va_list ap; va_start(ap, fmt);
	vsnprintf(m, sizeof m, fmt, ap);
	va_end(ap);
	if (strstr(g->warn, m)) return;
	if (!g->warn[0]) { snprintf(g->warn, sizeof g->warn, "%s", m); g->warn_line = g->line; return; }
	size_t n = strlen(g->warn);
	if (n + strlen(m) + 16 < sizeof g->warn) snprintf(g->warn + n, sizeof g->warn - n, "; line %d: %s", g->line + 1, m);
	else g->warn_more++;
}

/* ---- source lines and jumps ---- */

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

static size_t line_after(const gc_t *g, size_t off)
{
	while (off < g->len && g->text[off] != '\n') off++;
	return off < g->len ? off + 1 : off;
}

static int jump(gc_t *g, size_t off, int line)
{
	g->flow = true;
	if (++g->jumps > GC_MAX_JUMPS) return fail(g, "more than %d jumps - endless loop?", GC_MAX_JUMPS);
	g->cur = off; g->line = line - 1;
	return 0;
}

/* the line that starts with O<num> (also ":<num>") or N<num>, after an optional '/' */
static bool find_label(gc_t *g, char letter, int num, size_t *off, int *line)
{
	for (int i = 0; i < g->nlbl; i++)
		if (g->lbl[i].letter == letter && g->lbl[i].num == num) { *off = g->lbl[i].off; *line = g->lbl[i].line; return true; }
	size_t i = 0; int ln = 0;
	while (i < g->len) {
		const char *p = g->text + i, *e = g->text + g->len;
		while (p < e && (*p == ' ' || *p == '\t' || *p == '/')) p++;
		char c = p < e ? (char)toupper((unsigned char)*p) : 0;
		if (c == ':' && letter == 'O') c = 'O';
		if (c == letter) {
			p++;
			while (p < e && *p == ' ') p++;
			long n = 0; int nd = 0;
			while (p < e && isdigit((unsigned char)*p) && nd < 9) { n = n * 10 + (*p++ - '0'); nd++; }
			if (nd && n == num && !(p < e && (*p == '.' || isdigit((unsigned char)*p)))) {
				int k = g->nlbl < 16 ? g->nlbl++ : (int)(g->jumps % 16);
				g->lbl[k].letter = letter; g->lbl[k].num = num; g->lbl[k].off = i; g->lbl[k].line = ln;
				*off = i; *line = ln;
				return true;
			}
		}
		i = line_after(g, i); ln++;
	}
	return false;
}

/* M2 / M30 anywhere after the current position (outside comments) */
static bool rest_has_end(const gc_t *g)
{
	int depth = 0;
	for (size_t i = g->cur; i < g->len; i++) {
		char c = g->text[i];
		if (c == '\n') { depth = 0; continue; }
		if (c == '(') depth++;
		if (c == ')' && depth) { depth--; continue; }
		if (depth) continue;
		if (c == ';') { while (i < g->len && g->text[i] != '\n') i++; continue; }
		if (c == 'M' || c == 'm') {
			size_t j = i + 1;
			while (j < g->len && g->text[j] == ' ') j++;
			while (j < g->len && g->text[j] == '0') j++;
			bool two = j < g->len && g->text[j] == '2', thirty = j + 1 < g->len && g->text[j] == '3' && g->text[j + 1] == '0';
			size_t k = j + (two ? 1 : thirty ? 2 : 0);
			if ((two || thirty) && !(k < g->len && (isdigit((unsigned char)g->text[k]) || g->text[k] == '.'))) return true;
		}
	}
	return false;
}

/* ---- macro variables ---- */

static double inch(const gc_t *g) { return g->inches ? 25.4 : 1; }
static int wcs_idx(const gc_t *g) { return g->force_wcs >= 0 ? g->force_wcs : g->wcs; }
/* machine = program value + off(a): the work offset, G52, G92 and (Z) the tool length */
static double off(const gc_t *g, int a)
{
	double o = g->wcs_off[wcs_idx(g)][a];
	return g->shift ? o + g->g52[a] + g->g92[a] + (a == AX_Z ? g->tlo : 0) : o;   /* soft-float: skip the zeros */
}
static void reshift(gc_t *g)
{
	g->shift = g->tlo != 0;
	for (int a = 0; a < NAXES; a++) g->shift |= g->g52[a] != 0 || g->g92[a] != 0;
}
static double unit(const gc_t *g, int a) { return a == AX_A ? 1 : inch(g); }

static double getvar(gc_t *g, int n)
{
	if (n == 0) return VACANT;
	if (n >= 1 && n <= 33) return g->loc[g->lvl][n - 1];
	if (n >= 100 && n <= 199) return g->com[n - 100];
	if (n >= 500 && n <= 999) {
		if (!g->keep_warned) { g->keep_warned = true; warnf(g, "#500-#999 start vacant (not kept between runs)"); }
		return g->keep[n - 500];
	}
	if (n >= 2001 && n <= 2000 + NTOOLS) return g->tl_len[n - 2001] / inch(g);
	if (n >= 2401 && n <= 2400 + NTOOLS) return g->tl_dia[n - 2401] / 2 / inch(g);
	if (n == 3001 || n == 3002 || n == 3901 || n == 3902) return 0;
	if (n >= 4001 && n <= 4130) {
		switch (n) {
		case 4001: return g->cycle ? g->cycle : g->motion;
		case 4002: return g->plane;
		case 4003: return g->absolute ? 90 : 91;
		case 4005: return g->feedmode;
		case 4006: return g->inches ? 20 : 21;
		case 4007: return g->ccomp;
		case 4008: return g->tlo_code;
		case 4009: return g->cycle ? g->cycle : 80;
		case 4010: return g->retract_r ? 99 : 98;
		case 4012: return 67;
		case 4013: return 97;
		case 4014: return 54 + wcs_idx(g);
		case 4015: return g->exact ? 61 : 64;
		case 4016: return 69;
		case 4017: return 15;
		case 4109: return g->feedmode == 93 ? g->invf : g->feedmode == 95 ? g->feed_rev / inch(g) : g->feed / inch(g);
		case 4111: return 0;
		case 4114: return g->seq_no;
		case 4115: return g->prog_no;
		case 4119: return g->rpm;
		case 4120: return g->tool;
		}
	}
	if (n >= 5001 && n <= 5004) return (g->pos[n - 5001] - off(g, n - 5001)) / unit(g, n - 5001);
	if (n >= 5021 && n <= 5024) return g->pos[n - 5021] / unit(g, n - 5021);
	if (n >= 5041 && n <= 5044) return (g->pos[n - 5041] - off(g, n - 5041)) / unit(g, n - 5041);
	if (n == 5083) return g->tlo / inch(g);
	if (n >= 5201 && n <= 5204) return 0;
	if (n >= 5221 && n < 5221 + 20 * NWCS && (n - 5221) % 20 < NAXES) {
		int w = (n - 5221) / 20, a = (n - 5221) % 20;
		return g->wcs_off[w][a] / unit(g, a);
	}
	fail(g, "#%d not supported", n);
	return 0;
}

static void gc_pause(gc_t *g, int kind);

static void setvar(gc_t *g, int n, double v)
{
	if (n >= 1 && n <= 33) g->loc[g->lvl][n - 1] = v;
	else if (n >= 100 && n <= 199) g->com[n - 100] = v;
	else if (n >= 500 && n <= 999) g->keep[n - 500] = v;
	else if (n >= 2001 && n <= 2000 + NTOOLS) { g->tl_len[n - 2001] = v * inch(g); if (g->apply_g10) WD.tool[n - 2001].len = g->tl_len[n - 2001]; }
	else if (n == 3000) fail(g, "alarm #3000=%g%s%s", isnan(v) ? 0 : v, g->cmt[0] ? ": " : "", g->cmt);
	else if (n == 3006) { if (g->emit_now) gc_pause(g, PAUSE_M0); if (g->cmt[0]) warnf(g, "#3006 stop: %s", g->cmt); }
	else if ((n >= 3001 && n <= 3004) || n == 3901 || n == 3902) { /* timers, single block / hold suppression, part counts */ }
	else if (n >= 5221 && n < 5221 + 20 * NWCS && (n - 5221) % 20 < NAXES) {
		int w = (n - 5221) / 20, a = (n - 5221) % 20;
		g->wcs_off[w][a] = v * unit(g, a);
		if (g->apply_g10) WD.wcs[w].off[a] = g->wcs_off[w][a];
	} else fail(g, "#%d cannot be written", n);
}

/* ---- expressions: [ ] grouping, + - * / MOD, EQ NE GT GE LT LE, AND OR XOR, functions ---- */

static double nz(double v) { return isnan(v) ? 0 : v; }
static void ws(const char **pp) { while (**pp == ' ' || **pp == '\t' || **pp == '\r') (*pp)++; }
static bool kw(const char **pp, const char *k)
{
	const char *p = *pp; ws(&p);
	size_t n = strlen(k);
	if (strncasecmp(p, k, n)) return false;
	*pp = p + n;
	return true;
}

/* digits [. digits], no sign or exponent. Up to 15 significant digits it is exact: an integer
 * mantissa divided by an exact power of ten is correctly rounded - and cheap on the
 * controller's soft-float CPU (strtod made a 64 000-line load ~25 % slower) */
static bool number(const char **pp, double *v)
{
	static const double P10[] = { 1, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15 };
	const char *p = *pp, *s = p;
	unsigned long long m = 0;
	int sig = 0, frac = 0;
	bool dot = false, any = false, exact = true;
	for (;; p++) {
		if (*p >= '0' && *p <= '9') {
			any = true;
			if (sig < 15) { m = m * 10 + (unsigned)(*p - '0'); if (m) sig++; if (dot) frac++; }
			else exact = false;
		} else if (*p == '.' && !dot) dot = true;
		else break;
	}
	if (!any) return false;
	*pp = p;
	if (exact && frac <= 15) { *v = (double)m / P10[frac]; return true; }
	char b[64]; size_t n = 0;
	for (const char *q = s; q < p && n + 1 < sizeof b; q++) b[n++] = *q;
	b[n] = 0;
	*v = strtod(b, NULL);
	return true;
}

static double expr(gc_t *g, const char **pp);
#define D2R (M_PI / 180)

static double bracket(gc_t *g, const char **pp, double *second)
{
	ws(pp);
	if (**pp != '[') { fail(g, "'[' expected"); return 0; }
	(*pp)++;
	double v = expr(g, pp);
	ws(pp);
	if (second && **pp == ',') { (*pp)++; *second = expr(g, pp); ws(pp); }
	if (**pp != ']') { fail(g, "']' expected"); return 0; }
	(*pp)++;
	return v;
}

static double primary(gc_t *g, const char **pp)
{
	ws(pp);
	const char *p = *pp;
	double v;
	if (*p == '[') return bracket(g, pp, NULL);
	if (*p == '#') {
		(*pp)++;
		double i = primary(g, pp);
		if (g->bad) return 0;
		if (isnan(i) || i < 0 || i > 99999) { fail(g, "bad variable number"); return 0; }
		return getvar(g, (int)lround(i));
	}
	if (*p == '-') { (*pp)++; v = primary(g, pp); return isnan(v) ? v : -v; }
	if (*p == '+') { (*pp)++; return primary(g, pp); }
	if (number(pp, &v)) return v;
	static const char *const F[] = { "ASIN", "ACOS", "ATAN", "ATN", "SIN", "COS", "TAN", "SQRT", "SQR", "ABS",
		"ROUND", "RND", "FIX", "FUP", "LN", "EXP", "POW", "BIN", "BCD", "ADP", NULL };
	for (int f = 0; F[f]; f++) {
		const char *q = *pp;
		if (!kw(&q, F[f])) continue;
		const char *r = q; ws(&r);
		if (*r != '[') continue;
		*pp = q;
		double b = NAN, a = nz(bracket(g, pp, &b));
		if (g->bad) return 0;
		const char *s = F[f];
		if (!strcmp(s, "ATAN") || !strcmp(s, "ATN")) {
			const char *t = *pp; ws(&t);
			if (isnan(b) && *t == '/' ) { const char *u = t + 1; ws(&u); if (*u == '[') { *pp = u; b = bracket(g, pp, NULL); } }
			double r2 = isnan(b) ? atan(a) / D2R : atan2(a, nz(b)) / D2R;
			return isnan(b) ? r2 : (r2 < 0 ? r2 + 360 : r2);
		}
		if (!strcmp(s, "SIN")) return sin(a * D2R);
		if (!strcmp(s, "COS")) return cos(a * D2R);
		if (!strcmp(s, "TAN")) return tan(a * D2R);
		if (!strcmp(s, "ASIN")) return asin(CLAMP(a, -1.0, 1.0)) / D2R;
		if (!strcmp(s, "ACOS")) return acos(CLAMP(a, -1.0, 1.0)) / D2R;
		if (!strcmp(s, "SQRT") || !strcmp(s, "SQR")) { if (a < 0) { fail(g, "SQRT of a negative number"); return 0; } return sqrt(a); }
		if (!strcmp(s, "ABS")) return fabs(a);
		if (!strcmp(s, "ROUND") || !strcmp(s, "RND")) return round(a);
		if (!strcmp(s, "FIX")) return floor(a);
		if (!strcmp(s, "FUP")) return ceil(a);
		if (!strcmp(s, "LN")) { if (a <= 0) { fail(g, "LN of a number <= 0"); return 0; } return log(a); }
		if (!strcmp(s, "EXP")) return exp(a);
		if (!strcmp(s, "POW")) return pow(a, nz(b));
		if (!strcmp(s, "ADP")) return a;
		if (!strcmp(s, "BIN")) { long x = lround(a), r2 = 0, m = 1; while (x) { r2 += (x & 15) * m; x >>= 4; m *= 10; } return (double)r2; }
		if (!strcmp(s, "BCD")) { long x = lround(a), r2 = 0, m = 0; while (x) { r2 |= (x % 10) << m; x /= 10; m += 4; } return (double)r2; }
	}
	if (!*p) fail(g, "value expected");
	else fail(g, "unexpected '%.8s'", p);
	return 0;
}

static double mul(gc_t *g, const char **pp)
{
	double v = primary(g, pp);
	while (!g->bad) {
		ws(pp);
		if (**pp == '*') { (*pp)++; v = nz(v) * nz(primary(g, pp)); }
		else if (**pp == '/') {
			(*pp)++;
			double d = nz(primary(g, pp));
			if (d == 0) { fail(g, "division by zero"); return 0; }
			v = nz(v) / d;
		} else if (kw(pp, "MOD")) {
			double d = nz(primary(g, pp));
			if (d == 0) { fail(g, "MOD by zero"); return 0; }
			v = fmod(nz(v), d);
		} else break;
	}
	return v;
}

static double add(gc_t *g, const char **pp)
{
	double v = mul(g, pp);
	while (!g->bad) {
		ws(pp);
		if (**pp == '+') { (*pp)++; v = nz(v) + nz(mul(g, pp)); }
		else if (**pp == '-') { (*pp)++; v = nz(v) - nz(mul(g, pp)); }
		else break;
	}
	return v;
}

static double cmp(gc_t *g, const char **pp)
{
	double a = add(g, pp);
	static const char *const OPS[] = { "EQ", "NE", "GT", "GE", "LT", "LE" };
	for (int o = 0; o < 6 && !g->bad; o++) {
		if (!kw(pp, OPS[o])) continue;
		double b = add(g, pp);
		if (o < 2) {                                            /* vacant is only equal to vacant */
			bool eq = isnan(a) || isnan(b) ? isnan(a) && isnan(b) : fabs(a - b) < 1e-9;
			return (o == 0) == eq;
		}
		a = nz(a); b = nz(b);
		return o == 2 ? a > b : o == 3 ? a >= b : o == 4 ? a < b : a <= b;
	}
	return a;
}

static double land(gc_t *g, const char **pp)
{
	double v = cmp(g, pp);
	while (!g->bad && kw(pp, "AND")) v = (double)(lround(nz(v)) & lround(nz(cmp(g, pp))));
	return v;
}

static double expr(gc_t *g, const char **pp)
{
	double v = land(g, pp);
	while (!g->bad) {
		if (kw(pp, "OR")) v = (double)(lround(nz(v)) | lround(nz(land(g, pp))));
		else if (kw(pp, "XOR")) v = (double)(lround(nz(v)) ^ lround(nz(land(g, pp))));
		else break;
	}
	return v;
}

/* #n = expr (do = false: parsed, not stored) */
static void assign(gc_t *g, const char **pp, bool doit)
{
	ws(pp);
	if (**pp != '#') { fail(g, "'#' expected"); return; }
	(*pp)++;
	double i = primary(g, pp);
	ws(pp);
	if (g->bad) return;
	if (**pp != '=') { fail(g, "'=' expected after #%g", i); return; }
	(*pp)++;
	double v = expr(g, pp);
	ws(pp);
	if (g->bad) return;
	if (**pp) { fail(g, "unexpected '%.8s'", *pp); return; }
	if (isnan(i) || i < 1 || i > 99999) { fail(g, "bad variable number"); return; }
	if (doit) setvar(g, (int)lround(i), v);
}

/* ---- block parsing ---- */

typedef struct {
	double v[26]; bool has[26];
	int g[16], ng, m[8], nm;
	int nwords;                     /* words other than N and O */
	bool skip, pct, stmt;
	int jump;                       /* GOTO: the N to go to, -1 none */
	int while_m, end_m; bool while_ok;
} words_t;

static int parse(gc_t *g, char *s, words_t *w)
{
	memset(w, 0, sizeof *w);
	w->jump = -1;
	g->bad = false;
	g->cmt[0] = 0;
	int depth = 0; size_t cn = 0;
	for (char *p = s; *p; p++) {                 /* blank out comments, keep the first for #3000 */
		if (*p == ';' && !depth) { *p = 0; break; }
		if (*p == '(') { if (!depth++) { *p = ' '; continue; } }
		if (depth) {
			if (*p == ')') depth--;
			else if (cn + 1 < sizeof g->cmt && g->cmt[cn] == 0 && (cn || *p != ' ')) { g->cmt[cn++] = *p; g->cmt[cn] = 0; }
			*p = ' ';
		}
	}
	const char *p = s;
	ws(&p);
	if (*p == '/') {                              /* block delete */
		if (SET.gc_block_delete) { w->skip = true; return 0; }
		p++;
	}
	{
		const char *q = p;
		while (*q == '%' || isspace((unsigned char)*q)) q++;
		if (!*q && strchr(p, '%')) { w->pct = true; return 0; }
	}
	while (*p) {
		while (*p && (isspace((unsigned char)*p) || *p == '%')) p++;
		if (!*p) break;
		if (*p == '#') { assign(g, &p, true); if (g->bad) return -1; w->stmt = true; break; }
		int L = toupper((unsigned char)*p);
		if (L == ':') L = 'O';
		if (isalpha(L) && isalpha((unsigned char)p[1])) {          /* a keyword */
			w->stmt = true;
			if (kw(&p, "IF")) {
				double c = bracket(g, &p, NULL);
				if (g->bad) return -1;
				if (kw(&p, "GOTO")) {
					double n = primary(g, &p);
					if (g->bad) return -1;
					if (nz(c) != 0) w->jump = (int)lround(nz(n));
				} else if (kw(&p, "THEN")) {
					assign(g, &p, nz(c) != 0);
					if (g->bad) return -1;
					break;
				} else return fail(g, "IF [...] needs GOTO or THEN");
			} else if (kw(&p, "GOTO")) {
				double n = primary(g, &p);
				if (g->bad) return -1;
				w->jump = (int)lround(nz(n));
			} else if (kw(&p, "WHILE")) {
				double c = bracket(g, &p, NULL);
				if (g->bad) return -1;
				if (!kw(&p, "DO")) return fail(g, "WHILE [...] needs DO1-DO3");
				double m = primary(g, &p);
				if (g->bad) return -1;
				w->while_m = (int)lround(nz(m)); w->while_ok = nz(c) != 0;
				if (w->while_m < 1 || w->while_m > 3) return fail(g, "DO%d: only DO1-DO3", w->while_m);
			} else if (kw(&p, "END")) {
				double m = primary(g, &p);
				if (g->bad) return -1;
				w->end_m = (int)lround(nz(m));
				if (w->end_m < 1 || w->end_m > 3) return fail(g, "END%d: only END1-END3", w->end_m);
			} else if (L == 'O') return fail(g, "LinuxCNC o-word subroutines are not supported (use O/M98/M99)");
			else return fail(g, "unexpected '%.8s'", p);
			continue;
		}
		if (L < 'A' || L > 'Z') return fail(g, "unexpected '%c'", *p);
		p++;
		ws(&p);
		if (!(isdigit((unsigned char)*p) || *p == '.' || *p == '-' || *p == '+' || *p == '#' || *p == '[')) return fail(g, "%c without a number", L);
		double v = primary(g, &p);
		if (g->bad) return -1;
		if (isnan(v)) continue;                   /* a vacant variable: the word is left out (Fanuc) */
		if (L == 'G') { if (w->ng < 16) w->g[w->ng++] = (int)lround(v * 10); }
		else if (L == 'M') { if (w->nm < 8) w->m[w->nm++] = (int)lround(v); }
		else { w->v[L - 'A'] = v; w->has[L - 'A'] = true; }
		if (L != 'N' && L != 'O') w->nwords++;
	}
	return 0;
}

static bool hasm(const words_t *w, int code) { for (int i = 0; i < w->nm; i++) if (w->m[i] == code) return true; return false; }

/* ---- output ---- */

static gc_op_t *push(gc_t *g, op_kind_t k)
{
	gc_op_t *o = &g->q[g->qn++];
	memset(o, 0, sizeof *o);
	o->kind = k; o->line = g->line; o->feed = g->bfeed;
	memcpy(o->end, g->pos, sizeof o->end);
	return o;
}

static void gc_pause(gc_t *g, int kind) { push(g, OP_PAUSE)->ival = kind; }

static void line_to(gc_t *g, const double t[NAXES], bool rapid)
{
	gc_op_t *o = push(g, OP_LINE);
	o->rapid = rapid;
	memcpy(o->end, t, sizeof o->end);
	memcpy(g->pos, t, sizeof g->pos);
}

static void move(gc_t *g, const double t[NAXES], bool rapid, bool emit)
{
	if (emit) line_to(g, t, rapid); else memcpy(g->pos, t, sizeof g->pos);
}

static bool axis_word(const words_t *w, int a) { return w->has[AXL[a] - 'A']; }

/* target in machine coordinates for this block */
static void target(gc_t *g, const words_t *w, bool g53, double t[NAXES])
{
	for (int a = 0; a < NAXES; a++) {
		t[a] = g->pos[a];
		if (!axis_word(w, a)) continue;
		g->named |= (uint8_t)(1 << a);
		double v = w->v[AXL[a] - 'A'] * unit(g, a);
		if (g53) t[a] = v;
		else if (g->absolute) t[a] = v + off(g, a);
		else t[a] = g->pos[a] + v;
	}
}

/* the feed of a cutting move of length len (mm/min); -1 = error */
static int move_feed(gc_t *g, const words_t *w, double len)
{
	if (g->feedmode == 94) g->bfeed = g->feed;
	else if (g->feedmode == 95) {
		if (g->rpm <= 0) return fail(g, "G95 feed per revolution needs a spindle speed (S)");
		g->bfeed = g->feed_rev * g->rpm;
	} else {
		if (!w->has[W('F')] || g->invf <= 0) return fail(g, "G93 inverse time needs an F on every feed move");
		g->bfeed = len * g->invf;
	}
	return 0;
}

static int start_arc(gc_t *g, const words_t *w, const double t[NAXES], bool cw)
{
	int a0 = g->plane == 17 ? AX_X : g->plane == 18 ? AX_Z : AX_Y;
	int a1 = g->plane == 17 ? AX_Y : g->plane == 18 ? AX_X : AX_Z;
	int a2 = g->plane == 17 ? AX_Z : g->plane == 18 ? AX_Y : AX_X;
	double k = inch(g);
	double s0 = g->pos[a0], s1 = g->pos[a1], e0 = t[a0], e1 = t[a1], c0, c1;
	if (w->has['R' - 'A']) {
		double r = w->v['R' - 'A'] * k, dx = e0 - s0, dy = e1 - s1, d = hypot(dx, dy);
		if (d < 1e-9 || fabs(r) * 2 < d - 1e-6) return fail(g, "arc radius too small");
		double h = sqrt(fmax(0, r * r - d * d / 4)) * ((cw ^ (r < 0)) ? -1 : 1);
		c0 = s0 + dx / 2 - h * dy / d; c1 = s1 + dy / 2 + h * dx / d;
	} else {
		const char ci = g->plane == 17 ? 'I' : g->plane == 18 ? 'K' : 'J';
		const char cj = g->plane == 17 ? 'J' : g->plane == 18 ? 'I' : 'K';
		if (g->arc_abs) {                                   /* G90.1: the centre in work coordinates */
			if (!w->has[ci - 'A'] || !w->has[cj - 'A']) return fail(g, "G90.1 arc needs both centre words (%c %c)", ci, cj);
			c0 = w->v[ci - 'A'] * k + off(g, a0); c1 = w->v[cj - 'A'] * k + off(g, a1);
		} else { c0 = s0 + w->v[ci - 'A'] * k; c1 = s1 + w->v[cj - 'A'] * k; }
	}
	double r = hypot(s0 - c0, s1 - c1);
	if (r < 1e-6) return fail(g, "arc with zero radius");
	if (fabs(hypot(e0 - c0, e1 - c1) - r) > 0.05 + r * 1e-3) return fail(g, "arc end not on the circle");
	double st = atan2(s1 - c1, s0 - c0), en = atan2(e1 - c1, e0 - c0), sw = en - st;
	double eps = 0.01 / r;      /* end within 0.01 mm of the start = a full circle, whichever way CAM rounding went */
	if (cw && sw > -eps) sw -= 2 * M_PI;
	if (!cw && sw < eps) sw += 2 * M_PI;
	if (move_feed(g, w, hypot(r * sw, t[a2] - g->pos[a2])) < 0) return -1;
	double step = 2 * acos(fmax(-1.0, 1 - TOL / r));
	int n = (int)ceil(fabs(sw) / fmax(step, 1e-3));
	g->arc.on = true; g->arc.n = CLAMP(n, 1, 4000); g->arc.i = 0;
	g->arc.a0 = a0; g->arc.a1 = a1; g->arc.a2 = a2;
	g->arc.c0 = c0; g->arc.c1 = c1; g->arc.r = r; g->arc.start = st; g->arc.sweep = sw;
	g->arc.h0 = g->pos[a2]; g->arc.dh = t[a2] - g->pos[a2];
	g->arc.feed = g->bfeed;
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
	o->line = g->arc.line; o->arc = g->arc_seq; o->feed = g->arc.feed;
	memcpy(o->end, t, sizeof o->end);
	memcpy(g->pos, t, sizeof g->pos);
	if (!g->arc.on && g->arc.stop) { gc_op_t *d = push(g, OP_DWELL); d->line = g->arc.line; }   /* G9 / G61: exact stop */
}

/* Canned cycles. G81 drill, G82 + dwell, G83 peck (back out to R after each peck), G73 chip
 * break (back off PECK_CLEAR), G85 bore (feed out), G89 bore + dwell, G86 bore (spindle
 * stops at the bottom, rapid out, spindle on again), G76 fine bore (no spindle orientation
 * here: dwell, feed out like G89). Each hole: rapid up to R if below it, rapid XY, rapid to
 * R, cut, then rapid to the return level (G98: the Z the cycle started at, G99: R). K or L
 * repeats the hole, shifted by the X/Y increment in G91. */
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

/* the next few ops of the hole in progress (at most 4, so the op queue never overflows) */
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
		if ((c == 82 || c == 89 || c == 76) && g->cy_p > 0) push(g, OP_DWELL)->value = g->cy_p;
		if (c == 85 || c == 89 || c == 76) { p[AX_Z] = r; line_to(g, p, false); }
		if (c == 86 && g->spindle) push(g, OP_SPINDLE)->ival = 0;
		if (p[AX_Z] != g->hole.ret) { p[AX_Z] = g->hole.ret; line_to(g, p, true); }
		if (c == 86 && g->spindle) { gc_op_t *o = push(g, OP_SPINDLE); o->ival = g->spindle; o->value = g->rpm; }
		g->hole.on = false;
		if (g->hole.reps > 0) {                               /* K / L: the next one */
			g->hole.reps--;
			double t[NAXES];
			memcpy(t, g->pos, sizeof t);
			t[AX_X] += g->hole.inc[0]; t[AX_Y] += g->hole.inc[1];
			hole_start(g, t);
		}
	}
}

/* ---- one block ---- */

enum { NM_NONE, NM_G4, NM_G10, NM_G27, NM_G28, NM_G29, NM_G30, NM_G52, NM_G92, NM_G921, NM_G922, NM_G923 };

static int exec_block(gc_t *g, words_t *w, bool emit, bool *call65)
{
	int nonmodal = NM_NONE, newmotion = -1, newcycle = -1, tlc = -1;
	bool g9 = false, g53 = false, g80 = false;
	for (int i = 0; i < w->ng; i++) {
		int c = w->g[i];
		switch (c) {
		case 0: case 10: case 20: case 30: newmotion = c / 10; break;
		case 40: nonmodal = NM_G4; break;
		case 50:
			if (w->has[W('X')] || w->has[W('Y')] || w->has[W('I')]) return fail(g, "G5 spline not supported");
			warnf(g, "G5 ignored (high-precision mode)"); break;
		case 51: case 80: warnf(g, "G%g ignored (look-ahead / smoothing mode)", c / 10.0); break;
		case 70: case 71: return fail(g, "G%g not supported (lathe / cylindrical mode)", c / 10.0);
		case 90: g9 = true; break;
		case 100: nonmodal = NM_G10; break;
		case 110: case 150: case 670: case 690: case 500: case 501: case 970: break;
		case 400: g->ccomp = 40; break;
		case 120: case 130: return fail(g, "G%d circle pocket not supported", c / 10);
		case 160: return fail(g, "G16 polar coordinates not supported");
		case 170: case 180: case 190: g->plane = c / 10; break;
		case 200: g->inches = true; break;
		case 210: g->inches = false; break;
		case 270: nonmodal = NM_G27; break;
		case 280: nonmodal = NM_G28; break;
		case 290: nonmodal = NM_G29; break;
		case 300: nonmodal = NM_G30; break;
		case 281: case 301: warnf(g, "G%g ignored (G28/G30 go to the home position)", c / 10.0); break;
		case 310: case 382: case 383: case 384: case 385:
			return fail(g, "G%g probing not supported (no probe input)", c / 10.0);
		case 320: case 330: case 331: return fail(g, "G%g threading not supported (no spindle encoder)", c / 10.0);
		case 410: case 420: case 411: case 421:
			if (SET.gc_cutcomp != 1)
				return fail(g, "G41/G42 cutter comp not supported: post 'in computer', or 'wear' + Settings > G-code");
			g->ccomp = c / 10;
			warnf(g, "G41/G42 ignored: path cut as programmed (CAM wear mode)");
			break;
		case 430: case 440: case 490: tlc = c / 10; break;
		case 431: case 434: return fail(g, "G%g not supported", c / 10.0);
		case 510: return fail(g, "G51 scaling not supported");
		case 511: return fail(g, "G51.1 mirroring not supported");
		case 520: nonmodal = NM_G52; break;
		case 530: g53 = true; break;
		case 540: case 550: case 560: case 570: case 580: case 590: g->wcs = c / 10 - 54; break;
		case 541: case 591: case 592: case 593: return fail(g, "G%g: only G54-G59 exist here", c / 10.0);
		case 600: warnf(g, "G60 ignored (single-direction approach)"); break;
		case 610: g->exact = true; break;
		case 611: case 620: case 630: warnf(g, "G%g ignored", c / 10.0); break;
		case 640: g->exact = false; break;
		case 650: *call65 = true; break;
		case 660: return fail(g, "G66 modal macro call not supported");
		case 680: return fail(g, "G68 coordinate rotation not supported");
		case 730: case 760: case 810: case 820: case 830: case 850: case 860: case 890: newcycle = c / 10; break;
		case 740: case 840: case 841: case 842: case 843:
			return fail(g, "tapping G%g not supported (needs a spindle synced to the feed)", c / 10.0);
		case 870: return fail(g, "G87 back boring not supported (needs spindle orientation)");
		case 880: return fail(g, "G88 not supported (needs a manual retract)");
		case 800: g80 = true; break;
		case 900: g->absolute = true; break;
		case 910: g->absolute = false; break;
		case 901: g->arc_abs = true; break;
		case 911: g->arc_abs = false; break;
		case 920: nonmodal = NM_G92; break;
		case 921: nonmodal = NM_G921; break;
		case 922: nonmodal = NM_G922; break;
		case 923: nonmodal = NM_G923; break;
		case 930: case 940: case 950: g->feedmode = c / 10; break;
		case 960: return fail(g, "G96 constant surface speed not supported (program S in rpm, G97)");
		case 980: g->retract_r = false; break;
		case 990: g->retract_r = true; break;
		case 1870: warnf(g, "G187 ignored (smoothing)"); break;
		default: return fail(g, "G%g not supported", c / 10.0);
		}
	}
	if (*call65) return 0;                                   /* G65: the words are its arguments */
	double k = inch(g);
	if (w->has[W('F')]) {
		double f = w->v[W('F')];
		if (g->feedmode == 93) g->invf = f;
		else if (g->feedmode == 95) g->feed_rev = f * k;
		else g->feed = f * k;
	}
	if (w->has[W('S')]) g->rpm = w->v[W('S')];
	if (w->has[W('T')]) g->tool = (int)w->v[W('T')];
	if (w->has[W('N')]) g->seq_no = (int)w->v[W('N')];
	g->bfeed = g->feedmode == 95 ? g->feed_rev * g->rpm : g->feed;
	if (hasm(w, 6) && emit) { gc_op_t *o = push(g, OP_PAUSE); o->ival = PAUSE_TOOL; o->value = g->tool; }
	int sp = hasm(w, 3) ? 3 : hasm(w, 4) ? 4 : hasm(w, 5) ? 0 : -1;
	if (sp >= 0 || (w->has[W('S')] && g->spindle)) {
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
	static const int KNOWN_M[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 30, 60, 97, 98, 99 };
	for (int i = 0; i < w->nm; i++) {
		bool known = false;
		for (size_t j = 0; j < ARRAY_LEN(KNOWN_M); j++) known |= w->m[i] == KNOWN_M[j];
		if (!known) warnf(g, "M%d ignored", w->m[i]);
	}

	if (nonmodal == NM_G4) {                                 /* dwell: X seconds, or P (s or ms: Settings) */
		double s = 0;
		if (w->has[W('X')]) s = w->v[W('X')];
		else if (w->has[W('P')]) {
			s = w->v[W('P')] * (SET.gc_dwell_ms ? 0.001 : 1);
			if (!SET.gc_dwell_ms && s >= 100) warnf(g, "G4 P%g = %g s dwell (Fanuc P is ms: Settings > G-code)", w->v[W('P')], s);
		}
		if (emit && s > 0) push(g, OP_DWELL)->value = s;
		return 0;
	}

	int prev_cycle = g->cycle;
	if (newmotion >= 0) { g->motion = newmotion; g->cycle = 0; }
	if (g80) g->cycle = 0;                                   /* back to the G0/G1 mode from before */
	if (newcycle >= 0) {
		g->cycle = newcycle;
		if (newcycle == 76) warnf(g, "G76 without spindle orientation: dwell and feed out (like G89)");
	}

	if (tlc == 49) { g->tlo_code = 49; g->tlo = 0; reshift(g); }
	else if (tlc == 43 || tlc == 44) {
		g->tlo_code = tlc;
		int h = w->has[W('H')] ? (int)w->v[W('H')] : g->tool ? g->tool : WD.loaded_tool;
		if (!SET.gc_tlo) { g->tlo = 0; warnf(g, "G%d ignored: Z is touched off per tool (Settings > G-code)", tlc); }
		else {
			if (h < 0 || h > NTOOLS) return fail(g, "H%d: no such tool (T1-T%d)", h, NTOOLS);
			g->tlo = h ? (tlc == 43 ? 1 : -1) * g->tl_len[h - 1] : 0;
		}
		reshift(g);
	}

	bool any_axis = false;
	for (int a = 0; a < NAXES; a++) any_axis |= axis_word(w, a);
	double t[NAXES];

	switch (nonmodal) {
	case NM_G10: {                                           /* G10 L2 / L20 Pn: work offsets; L1 / L10-L13: tools */
		int l = (int)w->v[W('L')], p = (int)w->v[W('P')];
		if (l == 2 || l == 20) {
			int idx = p ? p - 1 : g->wcs;
			if (idx < 0 || idx >= NWCS) return fail(g, "G10 P%d out of range (P1-P%d)", p, NWCS);
			for (int a = 0; a < NAXES; a++) {
				if (!axis_word(w, a)) continue;
				double v = w->v[AXL[a] - 'A'] * unit(g, a);
				double rest = g->g52[a] + g->g92[a] + (a == AX_Z ? g->tlo : 0);
				if (l == 20) g->wcs_off[idx][a] = g->pos[a] - rest - v;
				else if (!g->absolute) g->wcs_off[idx][a] += v;           /* Fanuc: G91 G10 L2 adds */
				else g->wcs_off[idx][a] = v;
				if (g->apply_g10) WD.wcs[idx].off[a] = g->wcs_off[idx][a];
			}
		} else if (l == 1 || l == 10 || l == 12) {
			if (p < 1 || p > NTOOLS) return fail(g, "G10 L%d P%d: no such tool (1-%d)", l, p, NTOOLS);
			if (l == 1 && w->has[W('Z')]) g->tl_len[p - 1] = w->v[W('Z')] * k;
			if (l == 10 && w->has[W('R')]) g->tl_len[p - 1] = w->v[W('R')] * k;
			if ((l == 1 || l == 12) && w->has[W('R')]) g->tl_dia[p - 1] = 2 * w->v[W('R')] * k;
			if (g->apply_g10) { WD.tool[p - 1].len = g->tl_len[p - 1]; WD.tool[p - 1].dia = g->tl_dia[p - 1]; }
		} else if (l == 11 || l == 13) warnf(g, "G10 L%d ignored (no wear offsets)", l);
		else return fail(g, "G10 L%d not supported", l);
		return 0;
	}
	case NM_G92:                                             /* the current position reads the given values */
		if (!any_axis) return fail(g, "G92 needs an axis word");
		for (int a = 0; a < NAXES; a++) {
			if (!axis_word(w, a)) continue;
			g->g92[a] = 0; reshift(g);
			g->g92[a] = g->pos[a] - off(g, a) - w->v[AXL[a] - 'A'] * unit(g, a);
			g->g92_saved[a] = g->g92[a];
		}
		reshift(g);
		return 0;
	case NM_G921: memset(g->g92, 0, sizeof g->g92); memset(g->g92_saved, 0, sizeof g->g92_saved); reshift(g); return 0;
	case NM_G922: memset(g->g92, 0, sizeof g->g92); reshift(g); return 0;
	case NM_G923: memcpy(g->g92, g->g92_saved, sizeof g->g92); reshift(g); return 0;
	case NM_G52:
		for (int a = 0; a < NAXES; a++) if (axis_word(w, a)) g->g52[a] = w->v[AXL[a] - 'A'] * unit(g, a);
		reshift(g);
		return 0;
	case NM_G28: case NM_G30:                                /* via the named point, then those axes home */
		if (nonmodal == NM_G30) warnf(g, "G30 goes to the home position (no 2nd reference point)");
		if (any_axis) {
			target(g, w, false, t);
			move(g, t, true, emit);
			memcpy(g->mid, t, sizeof g->mid); g->mid_set = true;
			for (int a = 0; a < NAXES; a++) if (axis_word(w, a)) { t[a] = SET.ax[a].home_pos; g->named |= (uint8_t)(1 << a); }
			move(g, t, true, emit);
		} else {                                             /* no axis named: Z home first, then the rest */
			g->named = 0x0F;
			memcpy(t, g->pos, sizeof t);
			t[AX_Z] = SET.ax[AX_Z].home_pos;
			move(g, t, true, emit);
			for (int a = 0; a < NAXES; a++) t[a] = SET.ax[a].home_pos;
			move(g, t, true, emit);
		}
		return 0;
	case NM_G29:                                             /* from home: via the G28 point to the named one */
		if (!g->mid_set) return fail(g, "G29 without a G28 before it");
		if (!any_axis) return 0;
		memcpy(t, g->pos, sizeof t);
		for (int a = 0; a < NAXES; a++) if (axis_word(w, a)) t[a] = g->mid[a];
		move(g, t, true, emit);
		target(g, w, false, t);
		move(g, t, true, emit);
		return 0;
	case NM_G27:
		warnf(g, "G27 moves to the point but does not check it is home");
		if (!any_axis) return 0;
		target(g, w, false, t);
		move(g, t, true, emit);
		return 0;
	}

	if (g->cycle) {
		if (g->feedmode == 93) return fail(g, "canned cycles in G93 (inverse time) not supported");
		double zoff = off(g, AX_Z);
		if (!prev_cycle) { g->cy_init = g->cy_r = g->cy_z = g->pos[AX_Z]; g->cy_q = g->cy_p = 0; }
		/* R, Z, Q, P stay in force for the following holes; in G91, R is from the start level
		 * and Z from R */
		if (w->has['R' - 'A']) g->cy_r = w->v['R' - 'A'] * k + (g->absolute ? zoff : g->cy_init);
		if (w->has['Z' - 'A']) g->cy_z = w->v['Z' - 'A'] * k + (g->absolute ? zoff : g->cy_r);
		if (w->has['Q' - 'A']) g->cy_q = fabs(w->v['Q' - 'A'] * k);
		if (w->has['P' - 'A']) g->cy_p = w->v['P' - 'A'] * (SET.gc_dwell_ms ? 0.001 : 1);
		if (!any_axis) return 0;
		int reps = w->has[W('K')] ? (int)w->v[W('K')] : w->has[W('L')] ? (int)w->v[W('L')] : 1;
		if (reps <= 0) return 0;                             /* K0: the data only */
		if (g->cy_z >= g->cy_r - 1e-9) return fail(g, "G%d: hole bottom Z not below R", g->cycle);
		g->named |= 1 << AX_Z;
		target(g, w, false, t);                                 /* only its X/Y are used */
		double inc[2] = { 0, 0 };
		if (!g->absolute) { inc[0] = t[AX_X] - g->pos[AX_X]; inc[1] = t[AX_Y] - g->pos[AX_Y]; }
		if (!emit) {
			g->pos[AX_X] = t[AX_X] + inc[0] * (reps - 1); g->pos[AX_Y] = t[AX_Y] + inc[1] * (reps - 1);
			g->pos[AX_Z] = g->retract_r ? g->cy_r : fmax(g->cy_init, g->cy_r);
			return 0;
		}
		hole_start(g, t);
		g->hole.reps = reps - 1; g->hole.inc[0] = inc[0]; g->hole.inc[1] = inc[1];
		return 0;
	}
	if (!any_axis) return 0;
	target(g, w, g53, t);
	if (!emit) {                       /* seek: just track position */
		memcpy(g->pos, t, sizeof t);
		return 0;
	}
	bool stop = g9 || g->exact;
	if (g53 || g->motion == 0) line_to(g, t, true);
	else if (g->motion == 1) {
		double d = 0;
		for (int a = 0; a < NAXES; a++) d += (t[a] - g->pos[a]) * (t[a] - g->pos[a]);
		if (move_feed(g, w, sqrt(d)) < 0) return -1;
		line_to(g, t, false);
	} else if (g->motion == 2 || g->motion == 3) {
		if (start_arc(g, w, t, g->motion == 2) < 0) return -1;
		g->arc.stop = stop;
		return 0;
	}
	if (stop) push(g, OP_DWELL);       /* G9 / G61: a zero dwell makes the planner stop here */
	return 0;
}

/* M98 (O subprogram), M97 (N label, same program), G65 (macro with arguments) */
static int call(gc_t *g, int kind, const words_t *w)
{
	if (!w->has[W('P')]) return fail(g, "%s without P", kind == 65 ? "G65" : kind == 97 ? "M97" : "M98");
	if (g->sp >= GC_DEPTH) return fail(g, "calls nested deeper than %d", GC_DEPTH);
	long prog = lround(w->v[W('P')]);
	int reps = w->has[W('L')] ? (int)w->v[W('L')] : w->has[W('K')] && kind != 65 ? (int)w->v[W('K')] : 1;
	size_t at; int line;
	char letter = kind == 97 ? 'N' : 'O';
	bool found = prog >= 0 && find_label(g, letter, (int)prog, &at, &line);
	if (!found && kind == 98 && prog > 9999 && !w->has[W('L')]) {   /* Fanuc M98 P30100 = O0100 three times */
		reps = (int)(prog / 10000); prog %= 10000;
		found = find_label(g, 'O', (int)prog, &at, &line);
	}
	if (!found) return fail(g, "%s P%ld: no %c%ld in this file", kind == 65 ? "G65" : kind == 97 ? "M97" : "M98", prog, letter, prog);
	if (reps <= 0) return 0;
	if (letter == 'O') { at = line_after(g, at); line++; }
	int s = g->sp;
	g->st[s].ret = g->cur; g->st[s].ret_line = g->line + 1;
	g->st[s].sub = at; g->st[s].sub_line = line;
	g->st[s].reps = reps - 1; g->st[s].macro = kind == 65;
	if (kind == 65) {
		/* argument specification I: A B C I J K D E F H M Q R S T U V W X Y Z -> #1 ... #26 */
		static const signed char ARG[26] = { 1, 2, 3, 7, 8, 9, 0, 11, 4, 5, 6, 0, 13, 0, 0, 0, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26 };
		g->lvl++;
		for (int i = 0; i < 33; i++) g->loc[g->lvl][i] = VACANT;
		for (int L = 0; L < 26; L++) if (ARG[L] && w->has[L]) g->loc[g->lvl][ARG[L] - 1] = w->v[L];
		if (w->nm) g->loc[g->lvl][12] = w->m[0];
	}
	g->sp++;
	memset(g->dos[g->sp], 0, sizeof g->dos[g->sp]);
	return jump(g, at, line);
}

static int ret(gc_t *g, const words_t *w)
{
	if (g->sp == 0) {
		if (rest_has_end(g)) return fail(g, "M99 outside a subprogram: put the main program first");
		warnf(g, "M99 in the main program: ends it (no endless repeat)");
		g->cur = g->len;
		return 0;
	}
	int s = g->sp - 1;
	if (g->st[s].reps > 0) {
		g->st[s].reps--;
		memset(g->dos[g->sp], 0, sizeof g->dos[g->sp]);
		return jump(g, g->st[s].sub, g->st[s].sub_line);
	}
	if (g->st[s].macro) g->lvl--;
	g->sp--;
	if (jump(g, g->st[s].ret, g->st[s].ret_line) < 0) return -1;
	if (w->has[W('P')]) {                                     /* M99 Pn: back to N n of the caller */
		size_t at; int line;
		if (!find_label(g, 'N', (int)w->v[W('P')], &at, &line)) return fail(g, "M99 P%g: no N%g", w->v[W('P')], w->v[W('P')]);
		return jump(g, at, line);
	}
	return 0;
}

/* the line after END m (WHILE was false) */
static int skip_to_end(gc_t *g, int m)
{
	size_t i = g->cur; int ln = g->line + 1;
	while (i < g->len) {
		char b[256]; size_t n = 0; int depth = 0;
		size_t e = line_after(g, i);
		for (size_t j = i; j < e && n + 1 < sizeof b; j++) {
			char c = g->text[j];
			if (c == ';' && !depth) break;
			if (c == '(') depth++;
			else if (c == ')' && depth) depth--;
			else if (!depth && !isspace((unsigned char)c)) b[n++] = (char)toupper((unsigned char)c);
		}
		b[n] = 0;
		for (char *s = strstr(b, "END"); s; s = strstr(s + 1, "END")) {
			if (s > b && isalpha((unsigned char)s[-1])) continue;
			char *q = s + 3;
			if (atoi(q) == m && isdigit((unsigned char)*q)) return jump(g, e, ln + 1);
		}
		i = e; ln++;
	}
	return fail(g, "WHILE ... DO%d without END%d", m, m);
}

static int next_block(gc_t *g, bool emit)
{
	char buf[512];
	words_t w;
	size_t lstart = g->cur;
	if (!read_line(g, buf, sizeof buf)) {
		if (g->sp > 0) { fail(g, "end of file inside a subprogram (no M99)"); g->err_line = g->line; return -1; }
		return 0;
	}
	g->emit_now = emit;
	bool call65 = false;
	if (parse(g, buf, &w) < 0) goto bad;
	if (w.skip) return 1;
	if (w.pct) {                                              /* the closing % ends the tape */
		if (g->started) {
			if (g->sp > 0) { fail(g, "%% inside a subprogram (no M99)"); goto bad; }
			g->cur = g->len;
		}
		return 1;
	}
	if (w.has[W('O')]) {
		if (g->started && g->sp == 0) {                       /* the next program in the file */
			warnf(g, "main program ends at O%g (no M30)", w.v[W('O')]);
			g->cur = g->len;
			return 1;
		}
		if (g->started) { fail(g, "O%g reached inside a subprogram (no M99)", w.v[W('O')]); goto bad; }
		g->prog_no = (int)w.v[W('O')];
	}
	if (w.nwords || w.stmt) g->started = true;
	if (exec_block(g, &w, emit, &call65) < 0 || g->bad) goto bad;
	if (call65) { if (call(g, 65, &w) < 0) goto bad; return 1; }
	if (emit) {
		if (hasm(&w, 0) || hasm(&w, 60)) gc_pause(g, PAUSE_M0);
		if (hasm(&w, 1)) gc_pause(g, PAUSE_M1);
	}
	if (hasm(&w, 2) || hasm(&w, 30)) { if (emit) push(g, OP_END); g->cur = g->len; g->sp = 0; g->lvl = 0; return 1; }
	/* flow: at most one of these per block */
	if (w.end_m) {
		if (!g->dos[g->sp][w.end_m].on) { fail(g, "END%d without WHILE ... DO%d", w.end_m, w.end_m); goto bad; }
		if (jump(g, g->dos[g->sp][w.end_m].cur, g->dos[g->sp][w.end_m].line) < 0) goto bad;
	} else if (w.while_m) {
		if (w.while_ok) { g->dos[g->sp][w.while_m].cur = lstart; g->dos[g->sp][w.while_m].line = g->line; g->dos[g->sp][w.while_m].on = true; }
		else { g->dos[g->sp][w.while_m].on = false; if (skip_to_end(g, w.while_m) < 0) goto bad; }
	} else if (w.jump >= 0) {
		size_t at; int line;
		if (!find_label(g, 'N', w.jump, &at, &line)) { fail(g, "GOTO %d: no N%d", w.jump, w.jump); goto bad; }
		if (jump(g, at, line) < 0) goto bad;
	} else if (hasm(&w, 98) || hasm(&w, 97)) {
		if (call(g, hasm(&w, 98) ? 98 : 97, &w) < 0) goto bad;
	} else if (hasm(&w, 99)) {
		if (ret(g, &w) < 0) goto bad;
	}
	return 1;
bad:
	g->err_line = g->line;
	return -1;
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

void (*gc_yield)(void);

int gc_seek(gc_t *g, int line)
{
	unsigned n = 0;
	while (g->line + 1 != line) {                 /* != : after a jump the next line can be any */
		if (gc_yield && (++n & 1023) == 0) gc_yield();
		size_t before = g->cur;
		int r = next_block(g, false);
		if (r < 0) return -1;
		if (r == 0 || (g->cur == before && !g->flow) || g->cur >= g->len) break;
	}
	return 0;
}
