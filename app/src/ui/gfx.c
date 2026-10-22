#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"

void gfx_clip(fb_t *f, int x0, int y0, int x1, int y1)
{
	f->cx0 = CLAMP(x0, 0, f->w); f->cy0 = CLAMP(y0, 0, f->h);
	f->cx1 = CLAMP(x1, 0, f->w); f->cy1 = CLAMP(y1, 0, f->h);
}
void gfx_noclip(fb_t *f) { gfx_clip(f, 0, 0, f->w, f->h); }

static inline void put(fb_t *f, int x, int y, uint16_t c)
{
	if (x >= f->cx0 && x < f->cx1 && y >= f->cy0 && y < f->cy1) f->px[y * f->w + x] = c;
}

uint16_t gfx_mix(uint16_t bg, uint16_t fg, int a)
{
	uint32_t b = (bg | ((uint32_t)bg << 16)) & 0x07E0F81Fu;
	uint32_t g = (fg | ((uint32_t)fg << 16)) & 0x07E0F81Fu;
	uint32_t r = ((g * (uint32_t)a + b * (uint32_t)(16 - a)) >> 4) & 0x07E0F81Fu;
	return (uint16_t)(r | (r >> 16));
}

void gfx_dim(fb_t *f, int x, int y, int w, int h, int t)
{
	for (int yy = y > f->cy0 ? y : f->cy0; yy < y + h && yy < f->cy1; yy++)
		for (int xx = x > f->cx0 ? x : f->cx0; xx < x + w && xx < f->cx1; xx++)
			f->px[yy * f->w + xx] = gfx_mix(f->px[yy * f->w + xx], COL_BG1, t);
}

void gfx_fill(fb_t *f, int x, int y, int w, int h, uint16_t c)
{
	int x0 = x > f->cx0 ? x : f->cx0, x1 = x + w < f->cx1 ? x + w : f->cx1;
	int y0 = y > f->cy0 ? y : f->cy0, y1 = y + h < f->cy1 ? y + h : f->cy1;
	if (x0 >= x1) return;
	uint32_t c2 = (uint32_t)c << 16 | c;
	for (int yy = y0; yy < y1; yy++) {               /* two pixels per store: memory is the limit */
		uint16_t *p = f->px + yy * f->w + x0;
		int n = x1 - x0;
		if (((uintptr_t)p & 2) && n) { *p++ = c; n--; }
		uint32_t *q = (uint32_t *)p;
		for (int i = n >> 1; i; i--) *q++ = c2;
		if (n & 1) *(uint16_t *)q = c;
	}
}

void gfx_vgrad(fb_t *f, int x, int y, int w, int h, uint16_t top, uint16_t bot)
{
	for (int i = 0; i < h; i++) gfx_fill(f, x, y + i, w, 1, gfx_mix(top, bot, h > 1 ? i * 16 / (h - 1) : 0));
}

void gfx_hgrad(fb_t *f, int x, int y, int w, int h, uint16_t l, uint16_t r)
{
	for (int i = 0; i < w; i++) gfx_fill(f, x + i, y, 1, h, gfx_mix(l, r, w > 1 ? i * 16 / (w - 1) : 0));
}

void gfx_rect(fb_t *f, int x, int y, int w, int h, uint16_t c)
{
	gfx_fill(f, x, y, w, 1, c); gfx_fill(f, x, y + h - 1, w, 1, c);
	gfx_fill(f, x, y, 1, h, c); gfx_fill(f, x + w - 1, y, 1, h, c);
}

void gfx_panel(fb_t *f, int x, int y, int w, int h, bool focus)
{
	gfx_vgrad(f, x, y, w, h < 40 ? h : 40, COL_PANE, COL_PANE2);
	if (h > 40) gfx_fill(f, x, y + 40, w, h - 40, COL_PANE2);
	gfx_rect(f, x, y, w, h, focus ? COL_CYAN : COL_EDGE);
	if (focus) gfx_rect(f, x + 1, y + 1, w - 2, h - 2, gfx_mix(COL_PANE, COL_CYAN, 8));
	/* clipped corners read as rounded at this size */
	put(f, x, y, COL_BG1); put(f, x + w - 1, y, COL_BG1); put(f, x, y + h - 1, COL_BG1); put(f, x + w - 1, y + h - 1, COL_BG1);
}

void gfx_line(fb_t *f, int x0, int y0, int x1, int y1, uint16_t c, int width)
{
	int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
	if ((x0 < f->cx0 - 2000 && x1 < f->cx0) || (x0 > f->cx1 + 2000 && x1 > f->cx1)) return;
	for (int n = 0; n < 6000; n++) {
		put(f, x0, y0, c);
		if (width > 1) { put(f, x0 + 1, y0, c); put(f, x0, y0 + 1, c); if (width > 2) { put(f, x0 - 1, y0, c); put(f, x0, y0 - 1, c); } }
		if (x0 == x1 && y0 == y1) break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

void gfx_dash(fb_t *f, int x0, int y0, int x1, int y1, uint16_t c)
{
	int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
	for (int n = 0; n < 6000; n++) {
		if ((n & 7) < 4) put(f, x0, y0, c);
		if (x0 == x1 && y0 == y1) break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

void gfx_tri(fb_t *f, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c)
{
	int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2), maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
	int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2), maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
	for (int y = miny; y <= maxy; y++)
		for (int x = minx; x <= maxx; x++) {
			int w0 = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0);
			int w1 = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1);
			int w2 = (x0 - x2) * (y - y2) - (y0 - y2) * (x - x2);
			if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) put(f, x, y, c);
		}
}

void gfx_disc(fb_t *f, int cx, int cy, int r, uint16_t c)
{
	for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) if (x * x + y * y <= r * r + r) put(f, cx + x, cy + y, c);
}

void gfx_circle(fb_t *f, int cx, int cy, int r, uint16_t c)
{
	for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) {
		int d = x * x + y * y;
		if (d <= r * r + r && d >= (r - 1) * (r - 1) + (r - 1)) put(f, cx + x, cy + y, c);
	}
}

/* ---- text ---- */
static uint32_t utf8(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t c = *p++;
	if (c >= 0xF0) { c = (c & 7) << 18 | (p[0] & 63u) << 12 | (p[1] & 63u) << 6 | (p[2] & 63u); p += 3; }
	else if (c >= 0xE0) { c = (c & 15) << 12 | (p[0] & 63u) << 6 | (p[1] & 63u); p += 2; }
	else if (c >= 0xC0) { c = (c & 31) << 6 | (p[0] & 63u); p += 1; }
	*s = (const char *)p;
	return c;
}

static const glyph_t *glyph(const font_t *fn, uint32_t cp)
{
	int lo = 0, hi = fn->count - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (fn->cp[mid] == cp) return &fn->g[mid];
		if (fn->cp[mid] < cp) lo = mid + 1; else hi = mid - 1;
	}
	return cp == '?' ? NULL : glyph(fn, '?');
}

int gfx_text_w(const font_t *fn, const char *s)
{
	int w = 0;
	while (*s) { const glyph_t *g = glyph(fn, utf8(&s)); if (g) w += g->adv; }
	return w;
}

int gfx_text(fb_t *f, const font_t *fn, int x, int y, uint16_t c, const char *s)
{
	int x0 = x;
	while (*s) {
		const glyph_t *g = glyph(fn, utf8(&s));
		if (!g) continue;
		int gx = x + g->x, gy = y + g->y;
		for (int j = 0; j < g->h; j++) {
			int yy = gy + j;
			if (yy < f->cy0 || yy >= f->cy1) continue;
			for (int i = 0; i < g->w; i++) {
				int xx = gx + i;
				if (xx < f->cx0 || xx >= f->cx1) continue;
				int k = j * g->w + i;
				int a = (fn->bits[g->off + (k >> 1)] >> ((k & 1) ? 0 : 4)) & 15;
				if (!a) continue;
				uint16_t *p = &f->px[yy * f->w + xx];
				*p = a == 15 ? c : gfx_mix(*p, c, a + (a >> 3));
			}
		}
		x += g->adv;
	}
	return x - x0;
}

int gfx_text_r(fb_t *f, const font_t *fn, int xr, int y, uint16_t c, const char *s) { return gfx_text(f, fn, xr - gfx_text_w(fn, s), y, c, s); }
int gfx_text_c(fb_t *f, const font_t *fn, int xc, int y, uint16_t c, const char *s) { return gfx_text(f, fn, xc - gfx_text_w(fn, s) / 2, y, c, s); }

void gfx_text_fit(fb_t *f, const font_t *fn, int x, int y, int maxw, uint16_t c, const char *s)
{
	if (gfx_text_w(fn, s) <= maxw) { gfx_text(f, fn, x, y, c, s); return; }
	char buf[256];
	size_t n = strlen(s);
	if (n > sizeof buf - 4) n = sizeof buf - 4;
	memcpy(buf, s, n);
	while (n > 0) {
		do n--; while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80);
		memcpy(buf + n, "…", 4);
		if (gfx_text_w(fn, buf) <= maxw) break;
	}
	gfx_text(f, fn, x, y, c, buf);
}

/* Coral divider: a thin lacy band in the colours of the coral in the boot logo (orange, red,
 * magenta, violet, teal, green), generated once and blitted - a few thousand pixels per
 * frame. Pixels outside the lace are left alone, so it lies on any background. */
#define CORAL_W 1024
#define CORAL_H 7
static uint16_t coral_px[CORAL_H][CORAL_W];
static bool coral_on[CORAL_H][CORAL_W], coral_done;

static uint32_t chash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }

static void coral_make(void)
{
	static const uint8_t stops[][3] = { { 0xFF, 0x7A, 0x2F }, { 0xE8, 0x41, 0x3C }, { 0xC2, 0x33, 0x8F }, { 0x7B, 0x3F, 0xC4 },
		{ 0x1F, 0xA6, 0xA0 }, { 0x6D, 0xBE, 0x45 }, { 0xF2, 0xA7, 0x3B }, { 0xFF, 0x7A, 0x2F } };
	const int nst = 7, period = 420;
	for (int y = 0; y < CORAL_H; y++)
		for (int x = 0; x < CORAL_W; x++) {
			/* porous lace: rings around jittered hole centres on a 6 px grid */
			double best = 1e9;
			for (int i = x / 6 - 1; i <= x / 6 + 1; i++) {
				uint32_t h = chash((uint32_t)i * 2654435761u + 17);
				double cx = i * 6 + 1.5 + (h & 3), cy = 2.0 + ((h >> 3) % 3);
				double r = 1.2 + ((h >> 6) & 1) * 0.6;
				double d = sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r;
				if (d < best) best = d;
			}
			bool on = best > 0.15 && best < 1.9 && !(y == 0 && (chash((uint32_t)x) & 1)) && !(y == CORAL_H - 1 && (chash((uint32_t)x + 99) & 1));
			double t = fmod((double)x / period, 1.0) * nst;
			int k = (int)t; double f = t - k;
			double shade = 0.72 + 0.28 * (1.0 - fabs(best - 1.0));   /* rims a bit brighter */
			uint8_t c[3];
			for (int j = 0; j < 3; j++) c[j] = (uint8_t)fmin(255, (stops[k][j] + (stops[k + 1][j] - stops[k][j]) * f) * shade * 0.82);
			coral_on[y][x] = on;
			coral_px[y][x] = RGB(c[0], c[1], c[2]);
		}
	coral_done = true;
}

void gfx_coral(fb_t *f, int x, int y, int w)
{
	if (!coral_done) coral_make();
	for (int r = 0; r < CORAL_H; r++) {
		int yy = y + r;
		if (yy < f->cy0 || yy >= f->cy1) continue;
		uint16_t *row = f->px + yy * f->w;
		for (int i = 0; i < w; i++) {
			int xx = x + i;
			if (xx < f->cx0 || xx >= f->cx1) continue;
			int px = (xx) % CORAL_W;
			if (coral_on[r][px]) row[xx] = coral_px[r][px];
		}
	}
}
