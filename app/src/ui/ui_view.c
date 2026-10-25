/* Toolpath view: 2D plan or isometric WIREFRAME (lines only - no solids, no stock
 * removal: no GPU, no FPU). Turn is clamped between the X-Z and Y-Z side views and tilt
 * between side-on and the X-Y top view, so the view never flips past them. */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"

bool ui_view_key(pkey_t k)
{
	if (U.view.iso) {
		if (k == K_XM) U.view.yaw = fmax(-90, U.view.yaw - 15);
		else if (k == K_XP) U.view.yaw = fmin(0, U.view.yaw + 15);
		else if (k == K_YP) U.view.pitch = fmin(90, U.view.pitch + 8);
		else if (k == K_YM) U.view.pitch = fmax(0, U.view.pitch - 8);
		else goto common;
		return true;
	}
	if (k == K_XM) U.view.panx -= 10;
	else if (k == K_XP) U.view.panx += 10;
	else if (k == K_YP) U.view.pany += 10;
	else if (k == K_YM) U.view.pany -= 10;
	else goto common;
	return true;
common:
	if (k == K_CANCEL) U.view.zoom = fmin(8, U.view.zoom * 1.25);
	else if (k == K_ENTER) U.view.zoom = fmax(0.3, U.view.zoom / 1.25);
	else if (k == K_SEL) { U.view.yaw = -30; U.view.pitch = 32; U.view.zoom = 1; U.view.panx = U.view.pany = 0; }
	else return false;
	return true;
}

typedef struct { double cy, sy, ce, se, cx0, cy0, cz0, sc, zf; int ox, oy; bool iso; } proj_t;
void view_proj(proj_t *p, int x, int y, int w, int h);

static void P(const proj_t *p, double x, double y, double z, int *sx, int *sy)
{
	double dx = x - p->cx0, dy = y - p->cy0, dz = z - p->cz0;
	double xr = dx * p->cy - dy * p->sy, yr = dx * p->sy + dy * p->cy;
	*sx = p->ox + (int)lround(xr * p->sc);
	*sy = p->oy - (int)lround((yr * p->se + dz * p->zf * p->ce) * p->sc);   /* depth ×zf so Z reads */
}

/* The toolpath is drawn once into a cache (the panel background + stock + triad + path) and
 * blitted every frame; while running only the segments that became "done" since the last
 * frame are painted on. Measured on the unit: a 64 000-line program took 312 ms per frame
 * to draw from scratch. Anything that changes the picture rebuilds the cache. */
static struct {
	uint16_t *px;
	int x0, y0, x1, y1;                  /* cached rect (clipped) */
	double key[19];
	bool done_mode;
	int cur, vi;                         /* "done" painted up to line cur; next vertex vi */
	int pick, pvi;                       /* "picked" (orange) painted below line pick; next vertex pvi */
	bool valid;
} vc;

static void cache_io(fb_t *fb, bool save)
{
	for (int yy = vc.y0; yy < vc.y1; yy++) {
		uint16_t *f = fb->px + yy * fb->w + vc.x0, *c = vc.px + (yy - vc.y0) * (vc.x1 - vc.x0);
		if (save) memcpy(c, f, (size_t)(vc.x1 - vc.x0) * 2); else memcpy(f, c, (size_t)(vc.x1 - vc.x0) * 2);
	}
}

void ui_view_invalidate(void) { vc.valid = false; }

static void draw_view(fb_t *fb, int x, int y, int w, int h, bool mini);

void ui_view_render(fb_t *fb, int x, int y, int w, int h, bool mini)
{
	if (U.view.saver && !mini) { draw_view(fb, x, y, w, h, mini); return; }
	int sx0 = fb->cx0, sy0 = fb->cy0, sx1 = fb->cx1, sy1 = fb->cy1;
	int x0 = x > sx0 ? x : sx0, y0 = y > sy0 ? y : sy0, x1 = x + w < sx1 ? x + w : sx1, y1 = y + h < sy1 ? y + h : sy1;
	program_t *pg = &C.prog;
	bool running = C.m == M_RUN || C.m == M_HOLD;
	bool done_mode = running || C.work_done >= 0;
	int cur = done_mode ? ctl_cur_line() : -1;
	double jobs = WD.active * 1e6;                      /* the other JOB parts' zeros are drawn too */
	for (int i = 0; i < NWCS; i++) if (WD.wcs[i].job) jobs += (i + 1) * 7 + WD.wcs[i].off[0] * 3 + WD.wcs[i].off[1] * 5 + WD.wcs[i].off[2] * 11;
	double key[19] = { x0, y0, x1, y1, mini, U.view.iso, U.view.yaw, U.view.pitch, U.view.zoom, U.view.panx, U.view.pany,
		(double)(uintptr_t)pg->text, pg->npv, pg->loaded, U.screen, U.pane[U.screen], jobs, running, running ? C.run_from : -1 };   /* panel focus can tint the background */
	if (x1 <= x0 || y1 <= y0) return;
	if (!vc.px) vc.px = malloc(SCR_W * SCR_H * 2);
	int pick = ui_pick_line();
	/* scrolling down only paints more orange onto the cache; scrolling up redraws it */
	bool same = vc.px && vc.valid && !memcmp(key, vc.key, sizeof key) && vc.done_mode == done_mode && cur >= vc.cur && pick >= vc.pick;
	if (!same) {
		draw_view(fb, x, y, w, h, mini);                  /* full draw, without the tool marker */
		if (!vc.px) return;
		vc.x0 = x0; vc.y0 = y0; vc.x1 = x1; vc.y1 = y1;
		memcpy(vc.key, key, sizeof key); vc.done_mode = done_mode; vc.cur = cur; vc.valid = true;
		vc.vi = 0;
		while (vc.vi < pg->npv && pg->pv[vc.vi].line <= cur) vc.vi++;
		vc.pick = pick; vc.pvi = 0;
		while (vc.pvi < pg->npv && pg->pv[vc.pvi].line < pick) vc.pvi++;
		cache_io(fb, true);
	} else {
		cache_io(fb, false);
		if (cur > vc.cur) {                              /* paint the newly done segments */
			gfx_clip(fb, x0, y0, x1, y1);
			proj_t p;
			view_proj(&p, x, y, w, h);
			int ax, ay, bx, by;
			for (int i = vc.vi; i < pg->npv && pg->pv[i].line <= cur; i++, vc.vi++) {
				if (!i) continue;
				pv_t *a = &pg->pv[i - 1], *v = &pg->pv[i];
				if (v->rapid) continue;
				P(&p, a->x, a->y, a->z, &ax, &ay); P(&p, v->x, v->y, v->z, &bx, &by);
				gfx_line(fb, ax, ay, bx, by, COL_TEAL, !mini ? 2 : 1);
				if ((i & 511) == 511 && ui_yield) ui_yield();
			}
			vc.cur = cur;
			cache_io(fb, true);
			gfx_clip(fb, sx0, sy0, sx1, sy1);
		}
		if (pick > vc.pick) {                            /* scrolled further down: more orange */
			gfx_clip(fb, x0, y0, x1, y1);
			proj_t p;
			view_proj(&p, x, y, w, h);
			int ax, ay, bx, by;
			for (int i = vc.pvi; i < pg->npv && pg->pv[i].line < pick; i++, vc.pvi++) {
				if (!i) continue;
				pv_t *a = &pg->pv[i - 1], *v = &pg->pv[i];
				P(&p, a->x, a->y, a->z, &ax, &ay); P(&p, v->x, v->y, v->z, &bx, &by);
				if (v->rapid) gfx_dash(fb, ax, ay, bx, by, COL_PICK); else gfx_line(fb, ax, ay, bx, by, COL_PICK, !mini ? 2 : 1);
				if ((i & 511) == 511 && ui_yield) ui_yield();
			}
			vc.pick = pick;
			cache_io(fb, true);
			gfx_clip(fb, sx0, sy0, sx1, sy1);
		}
	}
	if (!mini) {                                         /* tool marker + view name: never cached */
		gfx_clip(fb, x0, y0, x1, y1);
		proj_t p;
		view_proj(&p, x, y, w, h);
		int ax, ay, bx, by;
		const double *o = WD.wcs[WD.active].off;
		P(&p, C.mo.pos[0] - o[0], C.mo.pos[1] - o[1], C.mo.pos[2] - o[2], &ax, &ay);
		if (p.iso) { P(&p, C.mo.pos[0] - o[0], C.mo.pos[1] - o[1], C.mo.pos[2] - o[2] + 8, &bx, &by); gfx_line(fb, ax, ay, bx, by, COL_TXT, 3); }
		gfx_disc(fb, ax, ay, 4, COL_TXT);
		if (pick >= 0 && pg->npv) {                      /* where the picked line takes the machine */
			int lo = 0, hi = pg->npv - 1;                /* last vertex with line <= pick */
			while (lo < hi) { int m = (lo + hi + 1) / 2; if (pg->pv[m].line <= pick) lo = m; else hi = m - 1; }
			pv_t *t = &pg->pv[lo];
			int tx, ty;
			P(&p, t->x, t->y, t->z, &tx, &ty);
			gfx_circle(fb, tx, ty, 7, COL_AMBER); gfx_circle(fb, tx, ty, 6, COL_AMBER);
			gfx_disc(fb, tx, ty, 3, COL_AMBER);
			char lb[24]; snprintf(lb, sizeof lb, "line %d", pick + 1);
			gfx_text(fb, &font_12, tx + 10, ty - 8, COL_AMBER, lb);
		}
		const char *named = !p.iso ? "2D plan" : U.view.pitch >= 90 ? "X-Y top view (limit)" : U.view.pitch <= 0 && U.view.yaw >= 0 ? "X-Z side view (limit)"
			: U.view.pitch <= 0 && U.view.yaw <= -90 ? "Y-Z side view (limit)" : "isometric wireframe";
		char b[96];
		snprintf(b, sizeof b, "%s  ·  ×%.1f", named, U.view.zoom);
		gfx_text(fb, &font_12, x + 8, y + h - 18, COL_DIM, b);
		gfx_clip(fb, sx0, sy0, sx1, sy1);
	}
}

/* the program's box (cutting moves), or a stand-in when nothing is loaded */
static void prog_box(double lo[3], double hi[3])
{
	program_t *pg = &C.prog;
	for (int a = 0; a < 3; a++) { lo[a] = pg->lo[a]; hi[a] = pg->hi[a]; }
	if (!pg->loaded || (hi[0] - lo[0] < 1e-3 && hi[1] - lo[1] < 1e-3)) { lo[0] = lo[1] = 0; hi[0] = 100; hi[1] = 70; lo[2] = -5; hi[2] = 0; }
}

/* the grid's height (work Z): the machine's Z floor (the Z soft limit -), when it is a
 * sensible distance below the program; else just under the program */
static double floor_z(const double lo[3])
{
	double z = SET.ax[AX_Z].soft_min - WD.wcs[WD.active].off[AX_Z];
	double low = fmin(lo[2], 0);
	return z <= low && z > low - 1000 ? z : low - 1;
}

/* the other JOB parts' zeros, relative to the active work zero; returns how many */
static int job_zeros(int idx[NWCS], double d[NWCS][3])
{
	int n = 0;
	const double *o = WD.wcs[WD.active].off;
	for (int i = 0; i < NWCS; i++) {
		if (i == WD.active || !WD.wcs[i].job) continue;
		for (int a = 0; a < 3; a++) d[n][a] = WD.wcs[i].off[a] - o[a];
		idx[n++] = i;
	}
	return n;
}

/* projection for a view rect (same maths as the full draw): everything that is drawn - the
 * program, its work zero and the other JOB parts' zeros - is projected and centred, so the
 * axes never stick out of the view (user 2026-10-23) */
void view_proj(proj_t *p, int x, int y, int w, int h)
{
	memset(p, 0, sizeof *p);
	p->iso = U.view.iso;
	double yaw = (p->iso ? U.view.yaw : 0) * M_PI / 180, el = (p->iso ? U.view.pitch : 90) * M_PI / 180;
	p->cy = cos(yaw); p->sy = sin(yaw); p->ce = cos(el); p->se = sin(el);
	double lo[3], hi[3], d[NWCS][3];
	int idx[NWCS];
	prog_box(lo, hi);
	if (C.prog.loaded && C.prog.mhi[2] > hi[2] && C.prog.mhi[2] - hi[2] < 500) hi[2] = C.prog.mhi[2];   /* its retracts too */
	for (int a = 0; a < 3; a++) { lo[a] = fmin(lo[a], 0); hi[a] = fmax(hi[a], 0); }
	int nj = job_zeros(idx, d);
	for (int k = 0; k < nj; k++) for (int a = 0; a < 3; a++) { lo[a] = fmin(lo[a], d[k][a]); hi[a] = fmax(hi[a], d[k][a]); }
	if (p->iso) lo[2] = fmin(lo[2], floor_z(lo));       /* the grid lies on the Z floor */
	/* Z is stretched (up to ×3) so a flat part reads, but never beyond the X-Y size */
	p->zf = CLAMP(fmax(hi[0] - lo[0], hi[1] - lo[1]) * 0.6 / fmax(hi[2] - lo[2], 1e-3), 1, 3);
	p->cx0 = (lo[0] + hi[0]) / 2 + (p->iso ? 0 : U.view.panx); p->cy0 = (lo[1] + hi[1]) / 2 + (p->iso ? 0 : U.view.pany); p->cz0 = (lo[2] + hi[2]) / 2;
	/* the box's corners at scale 1: their screen extent decides the scale and the centre */
	double ex0 = 1e30, ex1 = -1e30, ey0 = 1e30, ey1 = -1e30;
	for (int c = 0; c < 8; c++) {
		double dx = (c & 1 ? hi[0] : lo[0]) - p->cx0, dy = (c & 2 ? hi[1] : lo[1]) - p->cy0, dz = (c & 4 ? hi[2] : lo[2]) - p->cz0;
		double xr = dx * p->cy - dy * p->sy, yr = dx * p->sy + dy * p->cy, v = yr * p->se + dz * p->zf * p->ce;
		ex0 = fmin(ex0, xr); ex1 = fmax(ex1, xr); ey0 = fmin(ey0, v); ey1 = fmax(ey1, v);
	}
	int top = 58, bot = 22;                              /* the ARMED chip, % and estimate above, the view name below */
	double uw = w - 40, uh = h - top - bot - 16;
	p->sc = fmin(uw / fmax(ex1 - ex0, 1), uh / fmax(ey1 - ey0, 1)) * U.view.zoom;
	p->ox = x + w / 2 - (int)lround((ex0 + ex1) / 2 * p->sc);
	p->oy = y + top + (h - top - bot) / 2 + (int)lround((ey0 + ey1) / 2 * p->sc);
}

/* grid step: 1 cm for a picture under 10 cm, 5 cm up to 50 cm, 10 cm up to 1 m, then 50 cm / 1 m */
static double grid_step(double span) { return span < 100 ? 10 : span <= 500 ? 50 : span <= 1000 ? 100 : span <= 5000 ? 500 : 1000; }

/* a small X/Y(/Z) triad of `px` pixels at work position (x, y, z) */
static void triad(fb_t *fb, const proj_t *p, double x, double y, double z, int px, bool big, const char *label)
{
	double L = px / fmax(p->sc, 1e-9);
	int ax, ay, bx, by, wd = big ? 2 : 1;
	uint16_t cx = RGB(0xff, 0x7b, 0x7b), cy = RGB(0x5f, 0xe3, 0x9a), cz = COL_CYAN;
	if (!big) { cx = gfx_mix(COL_PANE2, cx, 10); cy = gfx_mix(COL_PANE2, cy, 10); cz = gfx_mix(COL_PANE2, cz, 10); }
	P(p, x, y, z, &ax, &ay);
	P(p, x + L, y, z, &bx, &by); gfx_line(fb, ax, ay, bx, by, cx, wd); if (big) gfx_text(fb, &font_12, bx + 3, by - 8, COL_DIM, "X");
	P(p, x, y + L, z, &bx, &by); gfx_line(fb, ax, ay, bx, by, cy, wd); if (big) gfx_text(fb, &font_12, bx + 3, by - 8, COL_DIM, "Y");
	if (p->iso) { P(p, x, y, z + L / p->zf, &bx, &by); gfx_line(fb, ax, ay, bx, by, cz, wd); if (big) gfx_text(fb, &font_12, bx + 3, by - 12, COL_DIM, "Z"); }
	if (label) gfx_text(fb, &font_12, ax + 4, ay + 2, COL_DIM, label);
}

static void draw_view(fb_t *fb, int x, int y, int w, int h, bool mini)
{
	if (U.view.saver && !mini) {
		/* CPU saver: no toolpath, no tool marker - motion gets the CPU. The screen stays on and
		 * the DRO still updates (once a second while moving, see main.c). */
		gfx_text(fb, &font_16, x + 16, y + h / 2 - 20, COL_DIM, "CPU saver - preview off");
		gfx_text(fb, &font_12, x + 16, y + h / 2 + 4, COL_DIM, "F4 View brings it back · an E-stop or limit switches to ISO");
		return;
	}
	int sx0 = fb->cx0, sy0 = fb->cy0, sx1 = fb->cx1, sy1 = fb->cy1;
	gfx_clip(fb, x > sx0 ? x : sx0, y > sy0 ? y : sy0, x + w < sx1 ? x + w : sx1, y + h < sy1 ? y + h : sy1);
	program_t *pg = &C.prog;
	proj_t p;
	view_proj(&p, x, y, w, h);
	double lo[3], hi[3];
	prog_box(lo, hi);
	int ax, ay, bx, by;
	/* grid on the machine's Z floor, aligned to work zero, covering the picture: coarse lines,
	 * and fine ones barely lighter than the background (user 2026-10-23) */
	double d[NWCS][3];
	int idx[NWCS], nj = job_zeros(idx, d);
	double gx0 = fmin(lo[0], 0), gx1 = fmax(hi[0], 0), gy0 = fmin(lo[1], 0), gy1 = fmax(hi[1], 0);
	for (int k = 0; k < nj; k++) { gx0 = fmin(gx0, d[k][0]); gx1 = fmax(gx1, d[k][0]); gy0 = fmin(gy0, d[k][1]); gy1 = fmax(gy1, d[k][1]); }
	double g = grid_step(fmax(gx1 - gx0, gy1 - gy0));
	gx0 = floor(gx0 / g - 1) * g; gx1 = ceil(gx1 / g + 1) * g; gy0 = floor(gy0 / g - 1) * g; gy1 = ceil(gy1 / g + 1) * g;
	double gz = p.iso ? floor_z(lo) : 0;
	if ((gx1 - gx0) / g < 80 && (gy1 - gy0) / g < 80) {
		double f = g * p.sc >= 50 ? g / 10 : g * p.sc >= 25 ? g / 5 : 0;   /* fine lines at least 5 px apart */
		if (f > 0 && (gx1 - gx0) / f < 400 && (gy1 - gy0) / f < 400) {
			uint16_t fc = RGB(0x0d, 0x27, 0x4b);
			for (double gx = gx0; gx <= gx1 + 1e-9; gx += f) { P(&p, gx, gy0, gz, &ax, &ay); P(&p, gx, gy1, gz, &bx, &by); gfx_line(fb, ax, ay, bx, by, fc, 1); }
			for (double gy = gy0; gy <= gy1 + 1e-9; gy += f) { P(&p, gx0, gy, gz, &ax, &ay); P(&p, gx1, gy, gz, &bx, &by); gfx_line(fb, ax, ay, bx, by, fc, 1); }
		}
		uint16_t gc = RGB(0x17, 0x37, 0x63);
		for (double gx = gx0; gx <= gx1 + 1e-9; gx += g) { P(&p, gx, gy0, gz, &ax, &ay); P(&p, gx, gy1, gz, &bx, &by); gfx_line(fb, ax, ay, bx, by, gc, 1); }
		for (double gy = gy0; gy <= gy1 + 1e-9; gy += g) { P(&p, gx0, gy, gz, &ax, &ay); P(&p, gx1, gy, gz, &bx, &by); gfx_line(fb, ax, ay, bx, by, gc, 1); }
		if (!mini) {
			char gl[80];
			if (f > 0) snprintf(gl, sizeof gl, "grid %g / %g mm%s", g, f, p.iso ? " · on the Z floor" : "");
			else snprintf(gl, sizeof gl, "grid %g mm%s", g, p.iso ? " · on the Z floor" : "");
			gfx_text_r(fb, &font_12, x + w - 8, y + h - 18, COL_DIM2, gl);
		}
	}
	/* stock box: the program's size (none without a program) */
	uint16_t sc = RGB(0x2a, 0x4b, 0x78);
	if (pg->loaded) {
		double bx0 = lo[0] - 3, bx1 = hi[0] + 3, by0 = lo[1] - 3, by1 = hi[1] + 3, bz[2] = { lo[2] < 0 ? lo[2] : -1, 0 };
		for (int i = 0; i < (p.iso ? 2 : 1); i++) {
			double xs[5] = { bx0, bx1, bx1, bx0, bx0 }, ys[5] = { by0, by0, by1, by1, by0 };
			for (int k = 0; k < 4; k++) { P(&p, xs[k], ys[k], bz[i], &ax, &ay); P(&p, xs[k + 1], ys[k + 1], bz[i], &bx, &by); gfx_line(fb, ax, ay, bx, by, sc, 1); }
		}
		if (p.iso) for (int k = 0; k < 4; k++) {
			double xs[4] = { bx0, bx1, bx1, bx0 }, ys[4] = { by0, by0, by1, by1 };
			P(&p, xs[k], ys[k], bz[0], &ax, &ay); P(&p, xs[k], ys[k], bz[1], &bx, &by); gfx_line(fb, ax, ay, bx, by, sc, 1);
		}
	}
	/* axis triad at work zero (a fixed size on screen), tiny ones at the other JOB parts' zeros */
	triad(fb, &p, 0, 0, 0, mini ? 16 : 26, true, NULL);
	for (int k = 0; k < nj; k++) {
		char gl[16]; snprintf(gl, sizeof gl, "G%d", 54 + idx[k]);
		triad(fb, &p, d[k][0], d[k][1], d[k][2], mini ? 8 : 12, false, gl);
	}
	/* toolpath */
	int cur = ctl_cur_line(), lx = 0, ly = 0;
	bool running = C.m == M_RUN || C.m == M_HOLD;
	int pick = ui_pick_line(), from = running || C.m == M_TOOL ? C.run_from : 0;
	for (int i = 0; i < pg->npv; i++) {
		/* a big program's toolpath takes longer than one 40 ms motion slice to draw:
		 * let motion run in between (measured: 45-55 ms for a 5000-line 3D program) */
		if ((i & 511) == 511 && ui_yield) ui_yield();
		pv_t *v = &pg->pv[i];
		int sx, sy;
		P(&p, v->x, v->y, v->z, &sx, &sy);
		if (i && (abs(sx - lx) + abs(sy - ly) >= 1 || i == pg->npv - 1)) {
			bool done = (running || C.work_done >= 0) && v->line <= cur && v->line >= from;
			bool pk = v->line < pick, skip = v->line < from;
			uint16_t c = pk ? COL_PICK : skip ? COL_SKIP : done ? COL_TEAL : RGB(0xc2, 0xd6, 0xf0);
			if (v->rapid) gfx_dash(fb, lx, ly, sx, sy, pk ? COL_PICK : skip ? COL_SKIP : RGB(0x3c, 0x7c, 0xa8));
			else gfx_line(fb, lx, ly, sx, sy, c, (done || pk) && !mini ? 2 : 1);
		}
		if (i == 0 || abs(sx - lx) + abs(sy - ly) >= 1) { lx = sx; ly = sy; }
	}
	gfx_clip(fb, sx0, sy0, sx1, sy1);
}
