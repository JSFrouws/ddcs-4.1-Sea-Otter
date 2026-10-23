/* Screen frame: top bar, banner, the 8 soft-key boxes (F1-F7 above their keys, the
 * 8th above GOTO 0 shows build date + pipeline id), the Tab/Shift/Mode strip, toast. */
#include <stdio.h>
#include <string.h>
#include "ui.h"

#ifndef BUILD_ID
#define BUILD_ID "dev"
#endif
#ifndef BUILD_DATE
#define BUILD_DATE "?"
#endif

static const char *state_word(uint16_t *col)
{
	*col = COL_GREEN;
	switch (C.m) {
	case M_RUN: *col = COL_CYAN; return C.run_mode == ARM_RUN ? "RUN" : C.run_mode == ARM_TRYCUT ? "TRYCUT" : "SIMULATE";
	case M_HOMING: *col = COL_CYAN; return "HOMING";
	case M_MOVE: *col = COL_CYAN; return "MOVE";
	case M_PROBING: *col = COL_CYAN; return "PROBING";
	case M_HOLD: *col = COL_AMBER; return "HOLD";
	case M_OFF: *col = COL_AMBER; return "DRIVES OFF";
	case M_LIMIT: *col = COL_RED; return "LIMIT";
	case M_TOOL: *col = COL_AMBER; return "TOOL CHANGE";
	case M_ESTOP: *col = COL_RED; return "E-STOP";
	default: return "READY";
	}
}

static void pill(fb_t *fb, int x, int y, const char *t, uint16_t c)
{
	int w = gfx_text_w(&font_12, t) + 18;
	gfx_fill(fb, x, y, w, 22, gfx_mix(COL_BG1, c, 3));
	gfx_rect(fb, x, y, w, 22, gfx_mix(COL_BG1, c, 8));
	gfx_text(fb, &font_12, x + 9, y + 3, c, t);
}

static void topbar(fb_t *fb)
{
	static const char *const titles[SCR__N] = { "Run", "Jog", "Files", "Work zero & probe", "Tools", "Settings", "Macros" };
	gfx_vgrad(fb, 0, 0, SCR_W, TOP_H, RGB(0x16, 0x44, 0x7e), RGB(0x0d, 0x2c, 0x56));
	gfx_fill(fb, 0, TOP_H - 1, SCR_W, 1, COL_EDGE2);
	gfx_coral(fb, 0, TOP_H - 6, SCR_W);                     /* coral divider under the top bar */
	const char *mode = U.screen == SCR_JOG ? "JOG" : U.screen == SCR_RUN ? "AUTO" : "SETUP";
	pill(fb, 10, 10, mode, COL_CYAN);
	int x = 10 + gfx_text_w(&font_12, mode) + 30;
	x += gfx_text(fb, &font_20, x, 9, COL_TXT, titles[U.screen]) + 10;
	char b[200];
	snprintf(b, sizeof b, "·  %s  ·  line %d / %d", C.prog.loaded ? C.prog.name : "no program", ctl_cur_line() + 1, C.prog.nlines);
	uint16_t sc;
	const char *sw = state_word(&sc);
	int sww = gfx_text_w(&font_14, sw) + 22;
	int jobs = 0;
	for (int i = 0; i < NWCS; i++) jobs += WD.wcs[i].job;
	char r[96];
	snprintf(r, sizeof r, "G%d%s   Homed %s   LAN %s", 54 + WD.active, jobs > 1 ? "  ·  job" : "",
		C.homed ? "XYZA" : "—", C.agent.on ? "agent" : "panel");
	int rw = gfx_text_w(&font_12, r);
	gfx_text_fit(fb, &font_14, x, 12, SCR_W - x - sww - rw - 40, COL_DIM, b);
	gfx_text(fb, &font_12, SCR_W - sww - rw - 22, 13, COL_DIM, r);
	gfx_fill(fb, SCR_W - sww - 10, 9, sww, 24, gfx_mix(COL_BG1, sc, 4));
	gfx_rect(fb, SCR_W - sww - 10, 9, sww, 24, gfx_mix(COL_BG1, sc, 9));
	gfx_text_c(fb, &font_14, SCR_W - sww / 2 - 10, 11, sc, sw);
}

int ui_banner_h(void)
{
	return (C.m == M_ESTOP || C.m == M_LIMIT || C.m == M_OFF || C.countdown || (C.m == M_HOLD && C.pause_msg[0]) || C.m == M_TOOL ||
		(C.m == M_RUN && C.tc_state != TC_NONE) || C.driver_warn[0] || C.macro_on) ? 30 : 0;
}

static void banner(fb_t *fb)
{
	if (!ui_banner_h()) return;
	char b[256];
	uint16_t c0 = COL_RED, tc = COL_TXT;
	if (C.m == M_ESTOP) snprintf(b, sizeof b, "E-STOP  ·  held by %s  ·  motion stopped, drives stay on  ·  release it to continue",
		C.stop_cause[0] ? C.stop_cause : "an E-stop input");
	else if (C.m == M_LIMIT) {
		/* every tripped switch: "X+ Y− LIMIT · jog X− Y+ to release · other axes free" */
		char on[48] = "", off[48] = "";
		bool unknown = false;
		for (int a = 0; a < NAXES; a++) {
			int d = C.lim_dir[a];
			if (!d) continue;
			if (d == 2) { unknown = true; snprintf(on + strlen(on), sizeof on - strlen(on), "%c ", AXIS_CH[a]); continue; }
			snprintf(on + strlen(on), sizeof on - strlen(on), "%c%s ", AXIS_CH[a], d > 0 ? "+" : "−");
			snprintf(off + strlen(off), sizeof off - strlen(off), "%c%s ", AXIS_CH[a], d > 0 ? "−" : "+");
		}
		if (unknown && !off[0]) snprintf(b, sizeof b, "%sLIMIT INPUT ACTIVE   direction unknown, both ways allowed  ·  Home, or check the switch level in Settings", on);
		else snprintf(b, sizeof b, "%sLIMIT   only those directions blocked  ·  jog %sto release  ·  other axes free", on, off);
	}
	else if (C.m == M_OFF) { c0 = COL_AMBER; tc = COL_BLACK; snprintf(b, sizeof b, "DRIVES OFF  ·  %s  ·  press RESET to switch the drives on",
		C.stop_cause[0] ? C.stop_cause : "switched off"); }
	else if (C.countdown) { c0 = RGB(0x1c, 0x6f, 0xd0); snprintf(b, sizeof b, "REMOTE START in %d s   requested over the API  ·  Reset cancels", C.countdown); }
	else if (C.m == M_HOLD && C.pause_msg[0]) { c0 = COL_AMBER; tc = COL_BLACK; snprintf(b, sizeof b, "%s", C.pause_msg); }
	else if (C.m == M_TOOL) {
		c0 = COL_AMBER; tc = COL_BLACK;
		char d[24] = "";
		if (C.tc_tool >= 1 && C.tc_tool <= NTOOLS && WD.tool[C.tc_tool - 1].dia > 0) { char v[16]; fmt_tool(v, sizeof v, WD.tool[C.tc_tool - 1].dia); snprintf(d, sizeof d, " (Ø%s)", v); }
		snprintf(b, sizeof b, "TOOL CHANGE  ·  put in T%d%s  ·  Mode = jog, touch off Z if needed  ·  F1 Start = back to the program  ·  F3 Reset stops", C.tc_tool, d);
	}
	else if (C.m == M_RUN && C.tc_state == TC_GOING) { c0 = COL_AMBER; tc = COL_BLACK; snprintf(b, sizeof b, "%s  ·  Z up first%s", C.pause_msg, SET.tc_where ? ", then the tool-change position" : ""); }
	else if (C.m == M_RUN && C.tc_state == TC_MACRO) { c0 = RGB(0x1a, 0x8f, 0x78); snprintf(b, sizeof b, "TOOL CHANGE  ·  T%d by the tool-change macro  ·  F2 Pause  ·  F3 Reset", C.tc_tool); }
	else if (C.macro_on) {
		c0 = RGB(0x1a, 0x8f, 0x78);                         /* teal: a macro instead of the program */
		if (C.m == M_RUN) snprintf(b, sizeof b, "MACRO  ·  %s running  ·  F2 Pause holds  ·  F3 Reset stops it", C.macro_name);
		else if (C.m == M_HOLD) snprintf(b, sizeof b, "MACRO  ·  %s on hold  ·  F1 Start continues  ·  F3 Reset stops it", C.macro_name);
		else snprintf(b, sizeof b, "MACRO  ·  %s armed  ·  F1 Start runs it  ·  F3 Reset cancels (your program comes back)", C.macro_name);
	}
	else snprintf(b, sizeof b, "UNTESTED FPGA DRIVER  ·  %.150s  ·  reinstall the Sea Otter package", C.driver_warn);   /* stays */
	gfx_hgrad(fb, 0, BODY_Y, CONTENT_W, 30, c0, gfx_mix(COL_BG1, c0, 6));
	gfx_text_fit(fb, &font_16, 12, BODY_Y + 5, CONTENT_W - 24, tc, b);
}

static void softkeys(fb_t *fb, const sk_t sk[7])
{
	int y = SCR_H - SK_H, w = SCR_W / 8;
	gfx_vgrad(fb, 0, y, SCR_W, SK_H, RGB(0x0d, 0x2c, 0x56), RGB(0x08, 0x1f, 0x3f));
	gfx_fill(fb, 0, y, SCR_W, 1, COL_EDGE2);
	gfx_coral(fb, 0, y - 7, SCR_W);                          /* and above the soft keys */
	uint16_t lc = U.shift ? COL_AMBER : U.pagesel ? COL_CYAN : COL_TXT, fc = U.shift ? COL_AMBER : COL_CYAN;
	if (U.pagesel) gfx_fill(fb, 0, y + 1, w * 7, SK_H - 1, RGB(0x0b, 0x3a, 0x52));   /* the screen menu is open */
	/* user 2026-10-23: keys that do nothing right now (the digit editor is open) are greyed out
	 * until barely visible; Pause / Reset stay lit while something moves */
	bool moving = ctl_busy() || !motion_idle(&C.mo);
	uint16_t off = RGB(0x1c, 0x3a, 0x60);
	for (int i = 0; i < 8; i++) {
		int x = i * w;
		if (i) gfx_fill(fb, x, y + 6, 1, SK_H - 12, COL_EDGE);
		bool dis = U.ed.on && i < 7 && !(moving && (i == 1 || i == 2));
		if (dis) {
			char f[4]; snprintf(f, sizeof f, "F%d", i + 1);
			gfx_text_c(fb, &font_12, x + w / 2, y + 2, off, f);
			gfx_text_c(fb, &font_16, x + w / 2, y + 15, off, sk[i].label ? sk[i].label : "—");
			if (sk[i].sub) gfx_text_c(fb, &font_12, x + w / 2, y + 34, off, sk[i].sub);
			continue;
		}
		if (i == 7) {                                     /* above GOTO 0: build info */
			gfx_text_c(fb, &font_12, x + w / 2, y + 3, COL_DIM2, "BUILD");
			gfx_text_c(fb, &font_12, x + w / 2, y + 17, COL_DIM, BUILD_DATE);
			gfx_text_c(fb, &font_12, x + w / 2, y + 32, COL_DIM, BUILD_ID);
			continue;
		}
		char f[4]; snprintf(f, sizeof f, "F%d", i + 1);
		gfx_text_c(fb, &font_12, x + w / 2, y + 2, fc, f);
		gfx_text_c(fb, &font_16, x + w / 2, y + 15, sk[i].label ? lc : COL_DIM2, sk[i].label ? sk[i].label : "—");
		if (sk[i].sub) gfx_text_c(fb, &font_12, x + w / 2, y + 34, COL_DIM, sk[i].sub);
	}
}

static void side_label(fb_t *fb, int cy, const char *key, const char *main, const char *sub, int style)
{
	int x = CONTENT_W + 4, w = SIDE_W - 4, h = 58, y = cy - h / 2;
	uint16_t a = style == 1 ? RGB(0x78, 0x50, 0x0a) : RGB(0x11, 0x34, 0x62);
	uint16_t b = style == 1 ? RGB(0xbe, 0x80, 0x14) : RGB(0x1d, 0x58, 0xa3);
	if (style == 2) { a = gfx_mix(COL_BG1, a, 8); b = gfx_mix(COL_BG1, b, 8); }
	gfx_hgrad(fb, x, y, w, h, a, b);
	gfx_rect(fb, x, y, w + 1, h, style == 1 ? COL_AMBER : COL_EDGE2);
	gfx_tri(fb, SCR_W - 9, cy - 6, SCR_W - 9, cy + 6, SCR_W - 3, cy, style == 1 ? COL_TXT : COL_CYAN);
	uint16_t off = RGB(0x2a, 0x4a, 0x72);                  /* style 2: not available right now */
	gfx_text(fb, &font_12, x + 8, y + 4, style == 1 ? RGB(0xff, 0xe2, 0xad) : style == 2 ? off : COL_CYAN, key);
	gfx_text_fit(fb, &font_16, x + 8, y + 18, w - 22, style == 2 ? off : COL_TXT, main);
	gfx_text_fit(fb, &font_12, x + 8, y + 38, w - 22, style == 1 ? RGB(0xff, 0xe2, 0xad) : style == 2 ? off : COL_DIM, sub);
}

static void side_strip(fb_t *fb)
{
	char tab[64], tsub[32], s1[64], s2[96];
	int n = ui_npanes(U.screen), p = ui_pane();
	if (n > 1) { snprintf(tab, sizeof tab, "→ %s", ui_pane_name(U.screen, p + 1)); snprintf(tsub, sizeof tsub, "pane %d/%d", p + 1, n); }
	else { snprintf(tab, sizeof tab, "—"); snprintf(tsub, sizeof tsub, "one pane"); }
	if (U.ed.on) { snprintf(s1, sizeof s1, "ALT"); snprintf(s2, sizeof s2, "ZERO = clear"); }
	else if (U.screen == SCR_JOG && p == 0) {
		snprintf(s1, sizeof s1, "touch-off");
		if (SET.edge_dia > 0) { char d[24]; fmt_tool(d, sizeof d, SET.edge_dia); snprintf(s2, sizeof s2, "edge Ø%s", d); }
		else snprintf(s2, sizeof s2, "axis = 0");
	} else if (U.screen == SCR_RUN || U.screen == SCR_JOG) { snprintf(s1, sizeof s1, "ALT"); snprintf(s2, sizeof s2, "2nd functions F1-F6"); }
	else { snprintf(s1, sizeof s1, "ALT"); snprintf(s2, sizeof s2, "reverse Tab, last screen"); }
	if (U.shift) { char t[96]; snprintf(t, sizeof t, "ON · %.80s", s2); snprintf(s2, sizeof s2, "%s", t); }
	bool lock = ctl_busy() && U.screen != SCR_JOG;
	side_label(fb, 166, "TAB", tab, tsub, U.ed.on ? 2 : 0);
	side_label(fb, 309, "SHIFT", s1, s2, U.shift ? 1 : 0);
	side_label(fb, 453, "MODE", U.screen == SCR_JOG ? "→ RUN" : lock ? "JOG locked" : "→ JOG", U.screen == SCR_JOG ? "auto" : "manual", lock || U.ed.on ? 2 : 0);
}

static void toast(fb_t *fb)
{
	if (!C.msg[0] || now_ms() - C.msg_t > (C.msg_bad ? 3500u : 2500u)) return;
	int w = gfx_text_w(&font_14, C.msg) + 28;
	if (w > CONTENT_W - 20) w = CONTENT_W - 20;
	int x = (CONTENT_W - w) / 2, y = SCR_H - SK_H - 40;
	gfx_fill(fb, x, y, w, 30, RGB(0x04, 0x0e, 0x1e));
	gfx_rect(fb, x, y, w, 30, C.msg_bad ? COL_RED : COL_CYAN);
	gfx_text_fit(fb, &font_14, x + 14, y + 6, w - 28, C.msg_bad ? RGB(0xff, 0xd3, 0xd3) : COL_TXT, C.msg);
}

/* M_TOOL, Start pressed once: what Start / Enter will do (user: ask before moving back) */
static void tc_prompt(fb_t *fb)
{
	if (C.m != M_TOOL || !C.tc_confirm) return;
	double t[NAXES], via;
	double v = ctl_tc_target(t, &via);
	int w = ctl_prog_wcs();
	const double *o = WD.wcs[w].off;
	int bw = 600, bh = 176, x = (CONTENT_W - bw) / 2, y = BODY_Y + 90;
	gfx_fill(fb, x, y, bw, bh, RGB(0x06, 0x14, 0x2a));
	gfx_rect(fb, x, y, bw, bh, COL_AMBER);
	gfx_rect(fb, x + 1, y + 1, bw - 2, bh - 2, COL_AMBER);
	char b[160], px[24], py[24], pz[24];
	gfx_text(fb, &font_20, x + 18, y + 12, COL_AMBER, "Back to the program?");
	fmt_pos(px, sizeof px, t[AX_X] - o[AX_X]); fmt_pos(py, sizeof py, t[AX_Y] - o[AX_Y]); fmt_pos(pz, sizeof pz, via - o[AX_Z]);
	snprintf(b, sizeof b, "Straight line to  X %s   Y %s   Z %s   (G%d)", px, py, pz, 54 + w);
	gfx_text_fit(fb, &font_16, x + 18, y + 48, bw - 36, COL_TXT, b);
	if (via > t[AX_Z] + 1e-9) { fmt_pos(pz, sizeof pz, t[AX_Z] - o[AX_Z]); snprintf(b, sizeof b, "then down to Z %s", pz); }
	else b[0] = 0;
	if (b[0]) gfx_text(fb, &font_16, x + 18, y + 72, COL_TXT, b);
	snprintf(b, sizeof b, "at %.0f mm/min (%.0f %% of rapid, Settings > Tool change), then T%d cuts on", v, SET.tc_return_pct, C.tc_tool);
	gfx_text_fit(fb, &font_14, x + 18, y + 98, bw - 36, COL_DIM, b);
	gfx_fill(fb, x + 12, y + bh - 44, bw - 24, 1, COL_EDGE);
	gfx_text(fb, &font_16, x + 18, y + bh - 34, COL_GREEN, "F1 Start / Enter = go");
	gfx_text_r(fb, &font_16, x + bw - 18, y + bh - 34, COL_DIM, "Cancel = stay here");
}

void ui_frame(fb_t *fb, const sk_t sk[7])
{
	topbar(fb);
	banner(fb);
	softkeys(fb, sk);
	side_strip(fb);
	tc_prompt(fb);
	toast(fb);
}
