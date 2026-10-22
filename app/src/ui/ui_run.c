/* Run (AUTO): DRO + overrides, toolpath view, program listing with resume picker. */
#include <stdio.h>
#include <string.h>
#include "ui.h"

static const char *const ARMED[] = { "Run", "TryCut", "Simulate" };

void dro_row(fb_t *fb, int x, int y, int w, int a, const font_t *big)
{
	char b[32], m[32];
	fmt_pos(b, sizeof b, ctl_work(a));
	fmt_pos(m, sizeof m, C.mo.pos[a]);
	gfx_text(fb, &font_20, x + 12, y + (big->height - 26) / 2 + 2, COL_CYAN, (char[]){ AXIS_CH[a], 0 });
	/* simulation: the coordinates are fictitious - orange, so nobody takes them for the machine */
	gfx_text_r(fb, big, x + w - 92, y, C.mo.dry ? COL_AMBER : COL_TXT, b);
	gfx_text_r(fb, &font_12, x + w - 12, y + big->ascent - 12, C.mo.dry ? COL_AMBER : COL_DIM, m);
}

/* All bars share one scale: full width = 200 %, so 100 % sits at the same x on every bar. The
 * overrides go to 300 % but the bar stops at 200 % (the number shows the rest, in amber);
 * Rapid tops out at 100 % and is drawn half width. */
#define BAR_MAX 200
static void bar(fb_t *fb, int x, int y, int w, const char *name, int v, int max)
{
	char b[16];
	gfx_text(fb, &font_14, x, y, COL_DIM, name);
	int bx = x + 62, bw = (w - 62 - 50) * max / BAR_MAX;
	gfx_fill(fb, bx, y + 7, bw, 6, RGB(0x1a, 0x30, 0x50));
	int fw = bw * CLAMP(v, 0, max) / max;
	gfx_hgrad(fb, bx, y + 7, fw, 6, RGB(0x2b, 0x8f, 0xe0), COL_CYAN);
	snprintf(b, sizeof b, "%d%%", v);
	gfx_text_r(fb, &font_14, x + w, y, v > max ? COL_AMBER : COL_TXT, b);
}

/* Feed: blue up to the selected override; while cutting, the part between the speed actually
 * applied right now (corner / braking for what's ahead) and the selected one turns orange-grey */
static void feed_bar(fb_t *fb, int x, int y, int w)
{
	bar(fb, x, y, w, "Feed", C.mo.feed_ov, 200);
	double ap = C.mo.applied_pct;
	if (ap < 0 || C.m != M_RUN) return;
	int bx = x + 62, bw = w - 62 - 50;
	int xs = bx + bw * CLAMP(C.mo.feed_ov, 0, 200) / 200, xa = bx + (int)(bw * CLAMP(ap, 0, 200) / 200);
	if (xs - xa >= 1) gfx_fill(fb, xa, y + 7, xs - xa, 6, RGB(0xb0, 0x86, 0x5c));
	gfx_fill(fb, xa, y + 4, 2, 12, RGB(0xe0, 0xa8, 0x70));       /* applied-speed marker */
}

static void agent_card(fb_t *fb, int x, int y, int w)
{
	agent_t *g = &C.agent;
	if (!g->on || !SET.agent_msgs) return;
	int h = 30 + g->n * 19 + 6;
	gfx_fill(fb, x, y - h, w, h, RGB(0x04, 0x16, 0x2c));
	gfx_rect(fb, x, y - h, w, h, COL_TEAL);
	char t[128];
	snprintf(t, sizeof t, "AGENT · %s · %s", g->who, g->title);
	gfx_text_fit(fb, &font_12, x + 10, y - h + 6, w - 20, COL_TEAL, t);
	for (int i = 0; i < g->n; i++) {
		int yy = y - h + 26 + i * 19;
		if (g->step[i].done) { gfx_line(fb, x + 11, yy + 8, x + 14, yy + 12, COL_GREEN, 2); gfx_line(fb, x + 14, yy + 12, x + 20, yy + 4, COL_GREEN, 2); }
		else gfx_circle(fb, x + 15, yy + 8, 5, COL_DIM2);
		gfx_text_fit(fb, &font_14, x + 28, yy, w - 38, g->step[i].done ? COL_DIM : COL_TXT, g->step[i].text);
	}
}

void run_render(fb_t *fb)
{
	int top = BODY_Y + ui_banner_h() + 8, bot = SCR_H - SK_H - 8, h = bot - top;
	/* DRO + overrides */
	int x = 8, w = 290;
	gfx_panel(fb, x, top, w, h, false);
	char b[96];
	snprintf(b, sizeof b, "G%d WORK  ·  MACHINE", 54 + WD.active);
	gfx_text(fb, &font_12, x + 10, top + 6, COL_DIM, b);
	for (int a = 0; a < NAXES; a++) dro_row(fb, x, top + 26 + a * 42, w, a, &font_28);
	int y = top + 26 + 4 * 42 + 6;
	gfx_text(fb, &font_12, x + 10, y, COL_DIM, "OVERRIDES  ·  PENDANT OFF + HOLD");
	feed_bar(fb, x + 12, y + 20, w - 24);
	bar(fb, x + 12, y + 42, w - 24, "Spindle", C.mo.spin_ov, 200);
	bar(fb, x + 12, y + 64, w - 24, "Rapid", rapid_override(C.mo.feed_ov), 100);
	y += 92;
	double f = C.m == M_RUN ? C.mo.v * 60 : 0;
	snprintf(b, sizeof b, "%.0f mm/min", f);
	gfx_text(fb, &font_14, x + 12, y, COL_DIM, "F"); gfx_text_r(fb, &font_14, x + w - 12, y, COL_TXT, b);
	snprintf(b, sizeof b, "%.0f rpm", C.spindle ? C.rpm * C.mo.spin_ov / 100.0 : 0);
	gfx_text(fb, &font_14, x + 12, y + 20, COL_DIM, "S"); gfx_text_r(fb, &font_14, x + w - 12, y + 20, COL_TXT, b);
	char d[24]; fmt_tool(d, sizeof d, WD.loaded_tool ? WD.tool[WD.loaded_tool - 1].dia : 0);
	snprintf(b, sizeof b, "T%d  ·  Ø%s", WD.loaded_tool, d);
	gfx_text(fb, &font_14, x + 12, y + 40, COL_DIM, "Tool"); gfx_text_r(fb, &font_14, x + w - 12, y + 40, COL_TXT, b);

	/* toolpath view */
	int vx = x + w + 8, vw = CONTENT_W - vx - 8, ph = 168, vh = h - ph - 8;
	gfx_panel(fb, vx, top, vw, vh, ui_pane() == 0);
	ui_view_render(fb, vx + 2, top + 2, vw - 4, vh - 4, false);
	snprintf(b, sizeof b, "ARMED · %s", ARMED[C.armed]);
	gfx_fill(fb, vx + 8, top + 8, gfx_text_w(&font_12, b) + 16, 20, RGB(0x3a, 0x2c, 0x10));
	gfx_text(fb, &font_12, vx + 16, top + 10, COL_AMBER, b);
	int cl = ctl_cur_line(), pct = C.prog.nlines ? (cl + 1) * 100 / C.prog.nlines : 0;
	snprintf(b, sizeof b, "%d%%", C.prog.loaded ? pct : 0);
	gfx_text_r(fb, &font_28, vx + vw - 10, top + 4, COL_TXT, b);
	int est = (int)C.prog.est_s;
	snprintf(b, sizeof b, "estimate %d:%02d:%02d", est / 3600, est / 60 % 60, est % 60);
	gfx_text_r(fb, &font_12, vx + vw - 10, top + 38, COL_DIM, b);
	agent_card(fb, vx + vw - 330, top + vh - 24, 320);

	/* program listing */
	int py = top + vh + 8;
	gfx_panel(fb, vx, py, vw, ph, ui_pane() == 1);
	int anchor = ui_pane() == 1 ? U.prog_line : cl;
	int first = CLAMP(anchor - 2, 0, C.prog.nlines > 6 ? C.prog.nlines - 6 : 0);
	for (int i = 0; i < 6 && first + i < C.prog.nlines; i++) {
		int n = first + i, ly = py + 6 + i * 20;
		bool cur = n == cl && (C.m == M_RUN || C.m == M_HOLD), done = n < cl || (C.work_done >= 0 && n <= C.work_done && !cur);
		if (cur) gfx_fill(fb, vx + 2, ly - 1, vw - 4, 20, RGB(0x17, 0x4a, 0x78));
		if (ui_pane() == 1 && n == U.prog_line) { gfx_fill(fb, vx + 2, ly - 1, vw - 4, 20, RGB(0x4a, 0x3a, 0x1c)); gfx_fill(fb, vx + 2, ly - 1, 3, 20, COL_AMBER); }
		if (n == C.resume_line) gfx_fill(fb, vx + 2, ly - 1, 3, 20, COL_AMBER);
		char ln[16], t[200];
		snprintf(ln, sizeof ln, "%d", n + 1);
		gfx_text_r(fb, &font_mono, vx + 52, ly, COL_DIM2, ln);
		program_line(&C.prog, n, t, sizeof t);
		gfx_text_fit(fb, &font_mono, vx + 62, ly, vw - 72, t[0] == '(' ? COL_TEAL : done ? COL_DIM : COL_TXT, t);
	}
	gfx_fill(fb, vx + 1, py + ph - 30, vw - 2, 1, COL_EDGE);
	if (C.resume_line >= 0) snprintf(b, sizeof b, "Resume @ line %d  ·  Start restores the modal state and approaches at safe Z", C.resume_line + 1);
	else if (C.work_done >= 0 && C.m != M_RUN) snprintf(b, sizeof b, "Work done up to line %d  ·  Tab → Program, Select on a line resumes there", C.work_done + 1);
	else snprintf(b, sizeof b, "Tab → Program:  Y± line  ·  X± 10 lines  ·  Select = resume here");
	gfx_text_fit(fb, &font_12, vx + 10, py + ph - 24, vw - 20, C.resume_line >= 0 ? COL_AMBER : COL_DIM, b);
}

void run_sk(sk_t sk[7])
{
	static char s1[24], s5[24];
	if (C.m == M_HOLD) snprintf(s1, sizeof s1, "resume");
	else if (C.resume_line >= 0) snprintf(s1, sizeof s1, "from %d", C.resume_line + 1);
	else if (C.macro_on) snprintf(s1, sizeof s1, "macro");
	else snprintf(s1, sizeof s1, "%s", C.armed == ARM_RUN ? "run" : C.armed == ARM_TRYCUT ? "dry run" : "simulate");
	snprintf(s5, sizeof s5, "armed: %s", ARMED[C.armed]);
	sk[0] = (sk_t){ "Start", s1 };
	sk[1] = (sk_t){ "Pause", "feed hold" };
	sk[2] = (sk_t){ "Reset", C.resume_line >= 0 ? "clear resume" : C.agent.on ? "close agent card" : "stop" };
	sk[3] = (sk_t){ "View", U.view.saver ? "→ 2D" : U.view.iso ? "→ CPU saver" : "→ ISO" };
	sk[4] = (sk_t){ "TryCut", s5 };
	sk[5] = (sk_t){ "Spindle", C.spindle ? "on" : "off" };
	sk[6] = (sk_t){ "Page R", U.shift ? "previous" : "Files" };
}

void run_key(pkey_t k, bool sh)
{
	if (sh) {                                              /* ALT: the second printed word */
		if (k == K_XM) { act(C_GOTOZERO, NULL, SRC_PANEL); return; }
		if (k == K_ENTER) { act(C_HOME, NULL, SRC_PANEL); return; }
		if (k == K_MOD) { act(C_PROBE, NULL, SRC_PANEL); return; }
		if (k == K_YM) {
			if (ctl_busy()) { ctl_toast(true, "Jog is locked while %s", STATE_NAMES[C.m]); return; }
			U.screen = SCR_JOG; U.pane[SCR_JOG] = 0; U.shift = true; return;
		}
	}
	switch (k) {
	case K_F1: act(C_START, NULL, SRC_PANEL); return;
	case K_F2: act(C_PAUSE, NULL, SRC_PANEL); return;
	case K_F3:
		if (C.resume_line >= 0) { C.resume_line = -1; ctl_toast(false, "Resume point cleared"); }
		else if (C.agent.on && motion_idle(&C.mo)) { C.agent.on = false; ctl_toast(false, "Agent instructions closed"); }
		else act(C_STOP, NULL, SRC_PANEL);
		return;
	case K_F4:                                         /* 2D -> ISO -> CPU saver -> 2D */
		if (U.view.saver) U.view.saver = false;
		else if (U.view.iso) { U.view.iso = false; U.view.saver = true; }
		else U.view.iso = true;
		return;
	case K_F5:
		if (ctl_busy()) { ctl_toast(true, "Arming can only change while idle"); return; }
		C.armed = (C.armed + 1) % 3;
		ctl_toast(false, "Armed: %s", C.armed == ARM_RUN ? "Run" : C.armed == ARM_TRYCUT ? "TryCut (spindle off, Z at safe height)" : "Simulate (no motion)");
		return;
	case K_F6: act(C_SPINDLE, NULL, SRC_PANEL); return;
	default: break;
	}
	if (ui_pane() == 0) {
		if (k == K_SEL && C.agent.on) {                    /* acknowledge the agent's next operator step */
			for (int i = 0; i < C.agent.n; i++)
				if (!C.agent.step[i].done && !strcmp(C.agent.step[i].check, "operator")) { C.agent.step[i].done = true; ctl_toast(false, "Confirmed: %s", C.agent.step[i].text); return; }
		}
		ui_view_key(k);
		return;
	}
	int n = C.prog.nlines;
	if (k == K_YP) U.prog_line = CLAMP(U.prog_line - 1, 0, n - 1);
	else if (k == K_YM) U.prog_line = CLAMP(U.prog_line + 1, 0, n - 1);
	else if (k == K_XM) U.prog_line = CLAMP(U.prog_line - 10, 0, n - 1);
	else if (k == K_XP) U.prog_line = CLAMP(U.prog_line + 10, 0, n - 1);
	else if (k == K_SEL || k == K_ENTER) {
		if (ctl_busy()) { ctl_toast(true, "Stop the program before choosing a resume line"); return; }
		C.resume_line = U.prog_line;
		ctl_toast(false, "Resume line %d - Start restores the modal state, approach at safe Z", U.prog_line + 1);
	} else if (k == K_CANCEL) { C.resume_line = -1; ctl_toast(false, "Resume point cleared"); }
}
