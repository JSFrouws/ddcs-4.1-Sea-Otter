#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "program.h"
#include "gcode.h"
#include "settings.h"

#define MAX_FILE (16u << 20)

void program_free(program_t *p)
{
	free(p->text); free(p->line_off); free(p->pv); free(p->goto_line);
	memset(p, 0, sizeof *p);
}

static void add_pv(program_t *p, const double m[NAXES], const double off[NAXES], int line, bool rapid, bool force)
{
	if (!p->pv) return;
	if (p->npv >= PREVIEW_MAX) {
		/* decimate: drop points of the same line, keep line changes */
		if (!force || p->pv[p->npv - 1].line == line) { pv_t *v = &p->pv[p->npv - 1];
			v->x = (float)(m[0] - off[0]); v->y = (float)(m[1] - off[1]); v->z = (float)(m[2] - off[2]); return; }
		return;
	}
	pv_t *v = &p->pv[p->npv++];
	v->x = (float)(m[0] - off[0]); v->y = (float)(m[1] - off[1]); v->z = (float)(m[2] - off[2]);
	v->line = line; v->rapid = rapid;
}

int program_load_text(program_t *p, const char *name, const char *text, size_t len)
{
	program_free(p);
	snprintf(p->name, sizeof p->name, "%s", name);
	p->text = malloc(len + 1);
	if (!p->text) { snprintf(p->err, sizeof p->err, "out of memory"); return -1; }
	memcpy(p->text, text, len);
	p->text[len] = 0; p->len = len;
	int n = 1;
	for (size_t i = 0; i < len; i++) if (text[i] == '\n') n++;
	p->line_off = malloc(sizeof(uint32_t) * (size_t)n);
	p->line_off[0] = 0; p->nlines = 1;
	for (size_t i = 0; i < len; i++) if (text[i] == '\n' && i + 1 <= len) p->line_off[p->nlines++] = (uint32_t)(i + 1);
	if (p->nlines > 1 && p->line_off[p->nlines - 1] >= len) p->nlines--;

	/* dry interpretation: validates the whole file and builds the preview */
	p->pv = malloc(sizeof(pv_t) * PREVIEW_MAX);
	const double *off = WD.wcs[WD.active].off;
	double start[NAXES];
	for (int a = 0; a < NAXES; a++) start[a] = off[a];
	start[AX_Z] = off[AX_Z] + SET.safe_z;
	gc_t g;
	gc_op_t op;
	gc_begin(&g, p->text, p->len, start);
	double last[NAXES];
	memcpy(last, start, sizeof last);
	add_pv(p, last, off, 0, true, true);
	for (int a = 0; a < 3; a++) { p->lo[a] = 1e30f; p->hi[a] = -1e30f; p->mlo[a] = 1e30f; p->mhi[a] = -1e30f; }
	int r;
	/* retracts: a rapid move up to a Z (work) - counted per height, the lines kept */
	enum { RMAX = 20000, ZMAX = 64 };
	int *rl = malloc(sizeof(int) * RMAX); float *rz = malloc(sizeof(float) * RMAX); int nr = 0;
	float zv[ZMAX]; int zc[ZMAX], nz = 0;
	double lastz = start[AX_Z] - off[AX_Z];
	while ((r = gc_next(&g, &op)) > 0) {
		if (op.kind == OP_LINE) {
			double z = op.end[AX_Z] - off[AX_Z];
			if (op.rapid && z > lastz + 0.01 && rl && rz && nr < RMAX) {
				float zr = (float)(floor(z * 100 + 0.5) / 100);
				rl[nr] = op.line; rz[nr++] = zr;
				int k = 0;
				while (k < nz && zv[k] != zr) k++;
				if (k == nz && nz < ZMAX) { zv[nz] = zr; zc[nz++] = 0; }
				if (k < nz) zc[k]++;
			}
			lastz = z;
		}
		if (op.kind == OP_DWELL) p->est_s += op.value;
		if (op.kind == OP_PAUSE && op.ival == PAUSE_TOOL && (!p->ntc || p->tc_line[p->ntc - 1] != op.line)) {
			if (p->ntc < PROG_MAX_TC) { p->tc_line[p->ntc] = op.line; p->tc_tool[p->ntc] = (int)op.value; }
			p->ntc++;
		}
		if (op.kind != OP_LINE) continue;
		double d = 0;
		for (int a = 0; a < 3; a++) d += (op.end[a] - last[a]) * (op.end[a] - last[a]);
		d = sqrt(d);
		p->est_s += d / ((op.rapid ? SET.ax[0].max_vel : fmax(op.feed, 1)) / 60.0);
		add_pv(p, op.end, off, op.line, op.rapid, !op.arc);
		for (int a = 0; a < 3; a++) {                       /* every move's end point */
			float w = (float)(op.end[a] - off[a]);
			if (w < p->mlo[a]) p->mlo[a] = w;
			if (w > p->mhi[a]) p->mhi[a] = w;
		}
		if (!op.rapid) for (int a = 0; a < 3; a++) {
			float w = (float)(op.end[a] - off[a]);
			if (w < p->lo[a]) p->lo[a] = w;
			if (w > p->hi[a]) p->hi[a] = w;
		}
		memcpy(last, op.end, sizeof last);
	}
	if (p->lo[0] > p->hi[0]) for (int a = 0; a < 3; a++) p->lo[a] = p->hi[a] = 0;
	if (p->mlo[0] > p->mhi[0]) for (int a = 0; a < 3; a++) p->mlo[a] = p->mhi[a] = 0;   /* no moves at all */
	/* goto points: sections if the program has them, else the retracts to its usual safe height */
	{
		enum { GMAX = 400 };
		p->goto_line = malloc(sizeof(int) * GMAX);
		int prev_comment = 1;                                /* the header block doesn't count */
		for (int i = 0; p->goto_line && i < p->nlines && p->ngoto < GMAX; i++) {
			const char *s = p->text + p->line_off[i];
			while (*s == ' ' || *s == '\t') s++;
			bool comment = *s == '(' || *s == ';';
			int letters = 0;
			for (const char *c = s; *c && *c != '\n' && letters < 3; c++) if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z')) letters++;
			if (comment && !prev_comment && letters >= 3) p->goto_line[p->ngoto++] = i;
			if (*s != '\n' && *s != '\r' && *s) prev_comment = comment;
		}
		p->goto_sections = p->ngoto >= 2;
		if (!p->goto_sections && p->goto_line) {
			p->ngoto = 0;
			int best = -1;                                   /* most frequent; ties: the highest */
			for (int k = 0; k < nz; k++) if (best < 0 || zc[k] > zc[best] || (zc[k] == zc[best] && zv[k] > zv[best])) best = k;
			if (best >= 0 && zc[best] >= 2) {
				p->goto_z = zv[best];
				for (int i = 0; i < nr && p->ngoto < GMAX; i++)
					if (rz[i] == zv[best] && (!p->ngoto || p->goto_line[p->ngoto - 1] != rl[i])) p->goto_line[p->ngoto++] = rl[i];
			}
		}
	}
	free(rl); free(rz);
	if (p->ntc > PROG_MAX_TC) {                             /* too many to cut per tool: whole program per part */
		p->ntc = 0;
		snprintf(p->warn, sizeof p->warn, "more than %d tool changes - a multi-part job runs the whole program per part", PROG_MAX_TC);
	}
	if (g.warn[0]) snprintf(p->warn, sizeof p->warn, "line %d: %s", g.warn_line + 1, g.warn);
	if (r < 0) {
		snprintf(p->err, sizeof p->err, "line %d: %s", g.err_line + 1, g.err);
		p->err_line = g.err_line;
		return -1;
	}
	p->loaded = true;
	return 0;
}

int program_load(program_t *p, const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) { snprintf(p->err, sizeof p->err, "cannot open %s", path); return -1; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0 || (unsigned long)n > MAX_FILE) { fclose(f); snprintf(p->err, sizeof p->err, "file too large"); return -1; }
	char *buf = malloc((size_t)n + 1);
	size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	const char *base = strrchr(path, '/');
	int r = program_load_text(p, base ? base + 1 : path, buf, got);
	snprintf(p->path, sizeof p->path, "%s", path);
	free(buf);
	return r;
}

void program_line(const program_t *p, int n, char *buf, size_t sz)
{
	buf[0] = 0;
	if (!p->text || n < 0 || n >= p->nlines) return;
	const char *s = p->text + p->line_off[n];
	size_t i = 0;
	while (s[i] && s[i] != '\n' && s[i] != '\r' && i + 1 < sz) { buf[i] = s[i] == '\t' ? ' ' : s[i]; i++; }
	buf[i] = 0;
}
