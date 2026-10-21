/* Jog (MANUAL): arrows jog, Shift + the key you approached with = touch-off
 * (with an edge Ø the axis becomes -dir*Ø/2; the same key again sets 0). */
#include <stdio.h>
#include <string.h>
#include "ui.h"

void dro_row(fb_t *fb, int x, int y, int w, int a, const font_t *big);

static const pkey_t JOGK[] = { K_XM, K_XP, K_YP, K_YM, K_CANCEL, K_ENTER, K_SEL, K_MOD };
static const int JOGA[] = { AX_X, AX_X, AX_Y, AX_Y, AX_Z, AX_Z, AX_A, AX_A };
static const int JOGD[] = { -1, 1, 1, -1, 1, -1, 1, -1 };
static int jog_index(pkey_t k) { for (int i = 0; i < 8; i++) if (JOGK[i] == k) return i; return -1; }

static double touch_value(int a, int dir)
{
	bool edge = (a == AX_X || a == AX_Y) && SET.edge_dia > 0;
	bool again = C.touch_axis == a && C.touch_dir == dir;
	return (!edge || again) ? 0 : -dir * SET.edge_dia / 2;
}

static void chip(fb_t *fb, int x, int y, const char *t, uint16_t c)
{
	int w = gfx_text_w(&font_12, t) + 14;
	gfx_fill(fb, x - w, y, w, 19, gfx_mix(COL_BG1, c, 4));
	gfx_text(fb, &font_12, x - w + 7, y + 2, c, t);
}

void jog_render(fb_t *fb)
{
	int top = BODY_Y + ui_banner_h() + 8, bot = SCR_H - SK_H - 8, h = bot - top;
	int x = 8, w = 604;
	bool sh = U.shift && ui_pane() == 0;
	gfx_panel(fb, x, top, w, h, ui_pane() == 0);
	char b[128];
	snprintf(b, sizeof b, "JOG  ·  G%d WORK / MACHINE  ·  %s", 54 + WD.active, sh ? "ALT: TOUCH-OFF VALUES" : "PER-DIRECTION STATUS");
	gfx_text(fb, &font_12, x + 10, top + 6, sh ? COL_AMBER : COL_DIM, b);
	int rh = (h - 30) / 4;
	for (int a = 0; a < NAXES; a++) {
		int y = top + 26 + a * rh;
		dro_row(fb, x, y, w - 150, a, &font_40);
		int cx = x + w - 12;
		for (int d = 1; d >= -1; d -= 2) {
			char t[48], v[24];
			snprintf(t, sizeof t, "%c%s", AXIS_CH[a], d > 0 ? "+" : "−");
			if (sh) {
				fmt_pos(v, sizeof v, touch_value(a, d));
				snprintf(t, sizeof t, "ALT %c%s → %s", AXIS_CH[a], d > 0 ? "+" : "−", v);
				chip(fb, cx, y + 14 + (d > 0 ? 0 : 22), t, COL_AMBER);
				continue;
			}
			arg_t j = { .axis = a, .dir = d, .dist = 0.001 };
			bool lim = C.lim_dir[a] == d;
			if (lim) snprintf(t, sizeof t, "%c%s LIMIT", AXIS_CH[a], d > 0 ? "+" : "−");
			chip(fb, cx, y + 14 + (d > 0 ? 0 : 22), t, can(C_JOG, &j, SRC_PANEL) ? COL_RED : COL_GREEN);
		}
		if (a < 3) gfx_fill(fb, x + 8, y + rh - 4, w - 16, 1, COL_EDGE);
	}
	/* setup pane */
	int sx = x + w + 8, sw = CONTENT_W - sx - 8;
	gfx_panel(fb, sx, top, sw, 150, ui_pane() == 1);
	gfx_text(fb, &font_12, sx + 10, top + 6, COL_DIM, "JOG SETUP  ·  Y± FIELD  ·  X± / ENTER");
	char v[4][48], d[24];
	if (SET.edge_dia > 0) { fmt_tool(d, sizeof d, SET.edge_dia); snprintf(v[0], sizeof v[0], "%s mm", d); }
	else snprintf(v[0], sizeof v[0], "none (= 0)");
	if (JOG_STEPS[U.step] > 0) snprintf(v[1], sizeof v[1], "%g mm", JOG_STEPS[U.step]); else snprintf(v[1], sizeof v[1], "continuous");
	snprintf(v[2], sizeof v[2], "G%d", 54 + WD.active);
	snprintf(v[3], sizeof v[3], "%.0f mm/min", SET.jog_feed);
	const char *lab[4] = { "Edge / tool Ø", "Step", "Touch-off sets", "Jog feed" };
	for (int i = 0; i < 4; i++) {
		int y = top + 26 + i * 30;
		if (ui_pane() == 1 && U.jog_field == i) { gfx_hgrad(fb, sx + 2, y - 3, sw - 4, 26, RGB(0x1f, 0x5a, 0x86), COL_PANE2); gfx_fill(fb, sx + 2, y - 3, 3, 26, COL_CYAN); }
		gfx_text(fb, &font_14, sx + 12, y, COL_TXT, lab[i]);
		gfx_text_r(fb, &font_16, sx + sw - 12, y - 1, COL_TXT, v[i]);
	}
	gfx_panel(fb, sx, top + 158, sw, h - 158, false);
	const char *hint[] = {
		"Jog: X−  X+  Y+  Y−  ·  Cancel Z+  ·  Enter Z−", "Select A+  ·  Modify A−",
		"Touch-off: Shift + the key you", "approached the part with.",
		SET.edge_dia > 0 ? "With an edge Ø: axis = −dir·Ø/2;" : "No Ø set: the axis becomes 0.",
		SET.edge_dia > 0 ? "same key again = 0 (centre)." : "", "Z touch-off always sets 0.",
		"Pendant: hold + X/Y/Z/4 jogs." };
	for (size_t i = 0; i < ARRAY_LEN(hint); i++) gfx_text(fb, &font_12, sx + 12, top + 166 + (int)i * 19, i == 2 || i == 0 ? COL_TXT : COL_DIM, hint[i]);
}

void jog_sk(sk_t sk[7])
{
	static char s3[16], s4[16], s5[16];
	if (JOG_STEPS[U.step] > 0) snprintf(s4, sizeof s4, "%g mm", JOG_STEPS[U.step]); else snprintf(s4, sizeof s4, "continuous");
	snprintf(s3, sizeof s3, "%s", C.m == M_OFF ? "→ ON" : "→ OFF");
	snprintf(s5, sizeof s5, "→ G%d", 54 + (WD.active + 1) % NWCS);
	static char s2[16]; snprintf(s2, sizeof s2, "G%d XY", 54 + WD.active);
	sk[0] = (sk_t){ "Home", "all axes" };
	sk[1] = (sk_t){ "Goto 0", s2 };
	sk[2] = (sk_t){ "Drives", s3 };
	sk[3] = (sk_t){ "Step", s4 };
	sk[4] = (sk_t){ "WCS", s5 };
	sk[5] = (sk_t){ "Spindle", C.spindle ? "on" : "off" };
	sk[6] = (sk_t){ "Page R", U.shift ? "previous" : "Files" };
}

static void set_edge(double v, int ctx) { SET.edge_dia = v; settings_save(C.cfg_path); }
static void set_feed(double v, int ctx) { SET.jog_feed = v; settings_save(C.cfg_path); }

void jog_key(pkey_t k, bool sh)
{
	switch (k) {
	case K_F1: act(C_HOME, NULL, SRC_PANEL); return;
	case K_F2: act(C_GOTOZERO, NULL, SRC_PANEL); return;
	case K_F3: act(C.m == M_OFF ? C_DRIVES_ON : C_DRIVES_OFF, NULL, SRC_PANEL); return;
	case K_F4: U.step = (U.step + 1) % NJOG_STEPS; return;
	case K_F5:
		if (ctl_busy()) { ctl_toast(true, "Change the WCS when idle"); return; }
		WD.active = (WD.active + 1) % NWCS; workdata_save(C.wd_path);
		ctl_toast(false, "Touch-off now sets G%d", 54 + WD.active); return;
	case K_F6: act(C_SPINDLE, NULL, SRC_PANEL); return;
	default: break;
	}
	int i = jog_index(k);
	if (ui_pane() == 0 && i >= 0) {
		arg_t a = { .axis = JOGA[i], .dir = JOGD[i] };
		if (sh) {
			act(C_TOUCHOFF, &a, SRC_PANEL);
			U.shift = C.touch_axis >= 0 && SET.edge_dia > 0;   /* stays lit: same key again = 0 */
			return;
		}
		a.dist = JOG_STEPS[U.step];
		a.cont = a.dist == 0;
		act(C_JOG, &a, SRC_PANEL);
		return;
	}
	if (ui_pane() != 1) return;
	if (k == K_YP || k == K_YM) { U.jog_field = CLAMP(U.jog_field + (k == K_YM ? 1 : -1), 0, 3); return; }
	int d = k == K_XP ? 1 : k == K_XM ? -1 : 0;
	if (U.jog_field == 1 && d) U.step = (U.step + d + NJOG_STEPS) % NJOG_STEPS;
	if (U.jog_field == 2 && d && !ctl_busy()) { WD.active = (WD.active + d + NWCS) % NWCS; workdata_save(C.wd_path); }
	if (k == K_ENTER || k == K_MOD || k == K_SEL) {
		if (U.jog_field == 0) ui_editor_open("Edge finder / tool Ø", SET.edge_dia, SET.tool_dec, "mm", 2, 50, false, false, set_edge, 0);
		if (U.jog_field == 3) {
			const char *w = can(C_EDIT_SETTING, NULL, SRC_PANEL);
			if (w) { ctl_toast(true, "%s", w); return; }
			ui_editor_open("Jog feed", SET.jog_feed, 0, "mm/min", 4, 6000, false, true, set_feed, 0);
		}
	}
}

/* continuous jog stops when the key is released */
void jog_key_release(pkey_t k)
{
	int i = jog_index(k);
	if (U.screen == SCR_JOG && i >= 0 && C.mo.jog_dir && C.mo.jog_axis == JOGA[i]) motion_jog_cont(&C.mo, 0, 0);
}
