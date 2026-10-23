/* Digit editor: one digit at a time under an underline cursor, like the vendor software.
 *   X-/X+  cursor left/right (left of the highest digit adds one; left of a leading zero -
 *          or of the 6th digit - is the sign cell, where Y+/Y- flip + / -)
 *   Modify flips the sign from anywhere (values that may be negative)
 *   Y+/Y-  digit +1/-1, wrapping 0<->9 with no carry: 0.0, two left, Y+ -> 100.0; Y- -> 900.0
 *   Enter/Select accept, Cancel discard, ALT+ZERO clear.  Pendant: wheel = cursor,
 *   hold + wheel = digit, quick press = accept. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"

void ui_editor_open(const char *label, double v, int dec, const char *unit, int intdig, double max, bool neg, bool locks,
		    void (*ok)(double, int), int ctx)
{
	char s[32];
	snprintf(s, sizeof s, "%.*f", dec, fabs(v));
	char *dot = strchr(s, '.');
	int ni = dot ? (int)(dot - s) : (int)strlen(s);
	memset(&U.ed, 0, sizeof U.ed);
	snprintf(U.ed.label, sizeof U.ed.label, "%s", label);
	snprintf(U.ed.unit, sizeof U.ed.unit, "%s", unit ? unit : "");
	int pad = intdig > ni ? intdig - ni : 0;
	int n = 0;
	for (int i = 0; i < pad; i++) U.ed.d[n++] = '0';
	for (int i = 0; s[i] && n < 11; i++) if (s[i] != '.') U.ed.d[n++] = s[i];
	U.ed.n = n; U.ed.nint = ni + pad; U.ed.cur = U.ed.nint - 1;
	U.ed.maxint = 6; U.ed.neg = v < 0; U.ed.allow_neg = neg; U.ed.max = max; U.ed.locks = locks;
	U.ed.ok = ok; U.ed.ctx = ctx; U.ed.on = true;
}

static double value(void)
{
	char s[24];
	int k = 0;
	for (int i = 0; i < U.ed.n; i++) { if (i == U.ed.nint) s[k++] = '.'; s[k++] = U.ed.d[i]; }
	s[k] = 0;
	return (U.ed.neg ? -1 : 1) * atof(s);
}

void ui_editor_turn(int n)
{
	if (U.ed.cur < 0) { U.ed.neg = !U.ed.neg; return; }
	int d = U.ed.d[U.ed.cur] - '0';
	U.ed.d[U.ed.cur] = (char)('0' + ((d + n) % 10 + 10) % 10);
}

void ui_editor_key(pkey_t k, bool sh)
{
	if (k == K_XM) {
		if (U.ed.cur > 0) U.ed.cur--;
		/* user 2026-10-23: the sign was only reachable after six digits - a leading zero
		 * already there means the next step left is the sign, not yet another zero */
		else if (U.ed.cur == 0 && U.ed.allow_neg && (U.ed.d[0] == '0' || U.ed.nint >= U.ed.maxint || U.ed.n >= 11)) U.ed.cur = -1;
		else if (U.ed.cur == 0 && U.ed.nint < U.ed.maxint && U.ed.n < 11) { memmove(U.ed.d + 1, U.ed.d, (size_t)U.ed.n); U.ed.d[0] = '0'; U.ed.n++; U.ed.nint++; }
	} else if (k == K_XP) { if (U.ed.cur < U.ed.n - 1) U.ed.cur++; }
	else if (k == K_YM && sh) { memset(U.ed.d, '0', (size_t)U.ed.n); U.ed.neg = false; }
	else if (k == K_MOD) { if (U.ed.allow_neg) U.ed.neg = !U.ed.neg; else ctl_toast(true, "This value can't be negative"); }
	else if (k == K_YP || k == K_YM) ui_editor_turn(k == K_YP ? 1 : -1);
	else if (k == K_ENTER || k == K_SEL) {
		double v = value();
		if (U.ed.max > 0 && v > U.ed.max) { v = U.ed.max; ctl_toast(false, "Limited to %g", U.ed.max); }
		U.ed.on = false;
		if (U.ed.locks && ctl_busy()) { ctl_toast(true, "Machine started moving - edit discarded"); return; }
		if (U.ed.ok) U.ed.ok(v, U.ed.ctx);
	} else if (k == K_CANCEL) { U.ed.on = false; ctl_toast(false, "Edit discarded"); }
}

void ui_editor_render(fb_t *fb)
{
	if (!U.ed.on) return;
	if (U.ed.locks && ctl_busy()) { U.ed.on = false; return; }
	int w = 460, h = 190, x = (CONTENT_W - w) / 2, y = BODY_Y + (BODY_H - h) / 2;
	gfx_dim(fb, 0, BODY_Y, CONTENT_W, BODY_H, 9);                    /* dim what's behind */
	gfx_vgrad(fb, x, y, w, 50, RGB(0x12, 0x3a, 0x6c), RGB(0x0a, 0x24, 0x48));
	gfx_fill(fb, x, y + 50, w, h - 50, RGB(0x0a, 0x24, 0x48));
	gfx_rect(fb, x, y, w, h, COL_AMBER);
	gfx_text_fit(fb, &font_14, x + 16, y + 10, w - 32, COL_DIM, U.ed.label);
	/* digits */
	const font_t *f = &font_40;
	int cw = gfx_text_w(f, "0"), dw = gfx_text_w(f, "."), sw = gfx_text_w(f, "−");
	int tw = (U.ed.allow_neg || U.ed.neg ? sw : 0) + U.ed.n * cw + (U.ed.n > U.ed.nint ? dw : 0);
	int cx = x + (w - tw) / 2 - 20, ty = y + 44;
	if (U.ed.allow_neg || U.ed.neg) {
		gfx_text(fb, f, cx, ty, U.ed.cur == -1 ? COL_AMBER : COL_TXT, U.ed.neg ? "−" : "+");
		if (U.ed.cur == -1) gfx_fill(fb, cx, ty + f->height - 2, sw, 4, COL_AMBER);
		cx += sw;
	}
	bool lead = true;
	for (int i = 0; i < U.ed.n; i++) {
		if (i == U.ed.nint) cx += gfx_text(fb, f, cx, ty, COL_TXT, ".");
		if (i == U.ed.nint - 1 || U.ed.d[i] != '0') lead = false;
		char c[2] = { U.ed.d[i], 0 };
		uint16_t col = i == U.ed.cur ? COL_AMBER : (lead ? COL_DIM2 : COL_TXT);
		gfx_text(fb, f, cx + (cw - gfx_text_w(f, c)) / 2, ty, col, c);
		if (i == U.ed.cur) gfx_fill(fb, cx + 2, ty + f->height - 2, cw - 4, 4, COL_AMBER);
		cx += cw;
	}
	gfx_text(fb, &font_20, cx + 12, ty + 18, COL_DIM, U.ed.unit);
	gfx_text_c(fb, &font_12, x + w / 2, y + h - 44, COL_DIM, U.ed.allow_neg ? "X−/X+ digit  ·  Y+/Y− change  ·  Modify = ± sign  ·  Enter accept  ·  Cancel discard"
		: "X−/X+ digit   ·   Y+/Y− change   ·   Enter accept   ·   Cancel discard   ·   ALT+ZERO clear");
	gfx_text_c(fb, &font_12, x + w / 2, y + h - 24, COL_DIM2, "pendant: wheel moves the cursor  ·  hold + wheel changes  ·  quick press accepts");
}
