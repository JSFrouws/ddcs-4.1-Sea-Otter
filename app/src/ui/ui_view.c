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

typedef struct { double cy, sy, ce, se, cx0, cy0, cz0, sc; int ox, oy; bool iso; } proj_t;
void view_proj(proj_t *p, int x, int y, int w, int h);

static void P(const proj_t *p, double x, double y, double z, int *sx, int *sy)
{
	double dx = x - p->cx0, dy = y - p->cy0, dz = z - p->cz0;
	double xr = dx * p->cy - dy * p->sy, yr = dx * p->sy + dy * p->cy;
	*sx = p->ox + (int)lround(xr * p->sc);
	*sy = p->oy - (int)lround((yr * p->se + dz * 3 * p->ce) * p->sc);   /* depth ×3 so Z reads */
}

/* The toolpath is drawn once into a cache (the panel background + stock + triad + path) and
 * blitted every frame; while running only the segments that became "done" since the last
 * frame are painted on. Measured on the unit: a 64 000-line program took 312 ms per frame
 * to draw from scratch. Anything that changes the picture rebuilds the cache. */
static struct {
	uint16_t *px;
	int x0, y0, x1, y1;                  /* cached rect (clipped) */
	double key[16];
	bool done_mode;
	int cur, vi;                         /* "done" painted up to line cur; next vertex vi */
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
	double key[16] = { x0, y0, x1, y1, mini, U.view.iso, U.view.yaw, U.view.pitch, U.view.zoom, U.view.panx, U.view.pany,
		(double)(uintptr_t)pg->text, pg->npv, pg->loaded, U.screen, U.pane[U.screen] };   /* panel focus can tint the background */
	if (x1 <= x0 || y1 <= y0) return;
	if (!vc.px) vc.px = malloc(SCR_W * SCR_H * 2);
	bool same = vc.px && vc.valid && !memcmp(key, vc.key, sizeof key) && vc.done_mode == done_mode && cur >= vc.cur;
	if (!same) {
		draw_view(fb, x, y, w, h, mini);                  /* full draw, without the tool marker */
		if (!vc.px) return;
		vc.x0 = x0; vc.y0 = y0; vc.x1 = x1; vc.y1 = y1;
		memcpy(vc.key, key, sizeof key); vc.done_mode = done_mode; vc.cur = cur; vc.valid = true;
		vc.vi = 0;
		while (vc.vi < pg->npv && pg->pv[vc.vi].line <= cur) vc.vi++;
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
		const char *named = !p.iso ? "2D plan" : U.view.pitch >= 90 ? "X-Y top view (limit)" : U.view.pitch <= 0 && U.view.yaw >= 0 ? "X-Z side view (limit)"
			: U.view.pitch <= 0 && U.view.yaw <= -90 ? "Y-Z side view (limit)" : "isometric wireframe";
		char b[96];
		snprintf(b, sizeof b, "%s  ·  ×%.1f", named, U.view.zoom);
		gfx_text(fb, &font_12, x + 8, y + h - 18, COL_DIM, b);
		gfx_clip(fb, sx0, sy0, sx1, sy1);
	}
}

/* projection for a view rect (same maths as the full draw) */
void view_proj(proj_t *p, int x, int y, int w, int h)
{
	program_t *pg = &C.prog;
	memset(p, 0, sizeof *p);
	p->iso = U.view.iso;
	double yaw = (p->iso ? U.view.yaw : 0) * M_PI / 180, el = (p->iso ? U.view.pitch : 90) * M_PI / 180;
	p->cy = cos(yaw); p->sy = sin(yaw); p->ce = cos(el); p->se = sin(el);
	double lo[3] = { pg->lo[0], pg->lo[1], pg->lo[2] }, hi[3] = { pg->hi[0], pg->hi[1], pg->hi[2] };
	if (!pg->loaded || hi[0] - lo[0] < 1) { lo[0] = lo[1] = 0; hi[0] = 100; hi[1] = 70; lo[2] = -5; hi[2] = 0; }
	p->cx0 = (lo[0] + hi[0]) / 2 + (p->iso ? 0 : U.view.panx); p->cy0 = (lo[1] + hi[1]) / 2 + (p->iso ? 0 : U.view.pany); p->cz0 = (lo[2] + hi[2]) / 2;
	double span = fmax(hi[0] - lo[0], hi[1] - lo[1]) * 1.25 + 1;
	p->sc = fmin(w, h) / span * U.view.zoom * (p->iso ? 0.85 : 1);
	p->ox = x + w / 2; p->oy = y + h / 2;
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
	double lo[3] = { pg->lo[0], pg->lo[1], pg->lo[2] }, hi[3] = { pg->hi[0], pg->hi[1], pg->hi[2] };
	if (!pg->loaded || hi[0] - lo[0] < 1) { lo[0] = lo[1] = 0; hi[0] = 100; hi[1] = 70; lo[2] = -5; hi[2] = 0; }
	int ax, ay, bx, by;
	/* stock box */
	uint16_t sc = RGB(0x2a, 0x4b, 0x78);
	double bx0 = lo[0] - 3, bx1 = hi[0] + 3, by0 = lo[1] - 3, by1 = hi[1] + 3, bz[2] = { lo[2] < 0 ? lo[2] : -1, 0 };
	for (int i = 0; i < (p.iso ? 2 : 1); i++) {
		double xs[5] = { bx0, bx1, bx1, bx0, bx0 }, ys[5] = { by0, by0, by1, by1, by0 };
		for (int k = 0; k < 4; k++) { P(&p, xs[k], ys[k], bz[i], &ax, &ay); P(&p, xs[k + 1], ys[k + 1], bz[i], &bx, &by); gfx_line(fb, ax, ay, bx, by, sc, 1); }
	}
	if (p.iso) for (int k = 0; k < 4; k++) {
		double xs[4] = { bx0, bx1, bx1, bx0 }, ys[4] = { by0, by0, by1, by1 };
		P(&p, xs[k], ys[k], bz[0], &ax, &ay); P(&p, xs[k], ys[k], bz[1], &bx, &by); gfx_line(fb, ax, ay, bx, by, sc, 1);
	}
	/* axis triad at work zero */
	P(&p, 0, 0, 0, &ax, &ay);
	P(&p, 15, 0, 0, &bx, &by); gfx_line(fb, ax, ay, bx, by, RGB(0xff, 0x7b, 0x7b), 2); gfx_text(fb, &font_12, bx + 3, by - 8, COL_DIM, "X");
	P(&p, 0, 15, 0, &bx, &by); gfx_line(fb, ax, ay, bx, by, RGB(0x5f, 0xe3, 0x9a), 2); gfx_text(fb, &font_12, bx + 3, by - 8, COL_DIM, "Y");
	if (p.iso) { P(&p, 0, 0, 5, &bx, &by); gfx_line(fb, ax, ay, bx, by, COL_CYAN, 2); gfx_text(fb, &font_12, bx + 3, by - 12, COL_DIM, "Z"); }
	/* toolpath */
	int cur = ctl_cur_line(), lx = 0, ly = 0;
	bool running = C.m == M_RUN || C.m == M_HOLD;
	for (int i = 0; i < pg->npv; i++) {
		/* a big program's toolpath takes longer than one 40 ms motion slice to draw:
		 * let motion run in between (measured: 45-55 ms for a 5000-line 3D program) */
		if ((i & 511) == 511 && ui_yield) ui_yield();
		pv_t *v = &pg->pv[i];
		int sx, sy;
		P(&p, v->x, v->y, v->z, &sx, &sy);
		if (i && (abs(sx - lx) + abs(sy - ly) >= 1 || i == pg->npv - 1)) {
			bool done = (running || C.work_done >= 0) && v->line <= cur;
			if (v->rapid) gfx_dash(fb, lx, ly, sx, sy, RGB(0x3c, 0x7c, 0xa8));
			else gfx_line(fb, lx, ly, sx, sy, done ? COL_TEAL : RGB(0xc2, 0xd6, 0xf0), done && !mini ? 2 : 1);
		}
		if (i == 0 || abs(sx - lx) + abs(sy - ly) >= 1) { lx = sx; ly = sy; }
	}
	gfx_clip(fb, sx0, sy0, sx1, sy1);
}
