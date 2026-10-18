/* Work zero & probe (G54-G59 table, multi-part job flags) and the Tools table. */
#include <stdio.h>
#include <string.h>
#include "ui.h"

static const char *const PROBES[] = { "Z touch plate", "Tool length (fixed sensor)", "Edge X / Y", "Outside corner", "Bore / boss centre" };

void work_render(fb_t *fb)
{
	int top = BODY_Y + ui_banner_h() + 8, bot = SCR_H - SK_H - 8;
	int x = 8, w = CONTENT_W - 16, th = 36 + NWCS * 30 + 8;
	gfx_panel(fb, x, top, w, th, ui_pane() == 0);
	gfx_text(fb, &font_12, x + 10, top + 6, COL_DIM, "WORK OFFSETS = MACHINE POSITION OF EACH PART'S ZERO  ·  Y± ROW  ·  X± COLUMN  ·  ENTER EDITS");
	const char *hd[] = { "WCS", "X", "Y", "Z", "A" };
	int cx[] = { x + 14, x + 230, x + 380, x + 530, x + 660 };
	for (int i = 0; i < 5; i++) {
		if (i) gfx_text_r(fb, &font_12, cx[i], top + 24, COL_DIM2, hd[i]); else gfx_text(fb, &font_12, cx[i], top + 24, COL_DIM2, hd[i]);
	}
	for (int r = 0; r < NWCS; r++) {
		int y = top + 42 + r * 30;
		bool f = ui_pane() == 0 && r == U.wcs_row;
		if (f) gfx_fill(fb, x + 2, y - 4, w - 4, 28, RGB(0x17, 0x4a, 0x78));
		char b[32];
		snprintf(b, sizeof b, "G%d", 54 + r);
		gfx_text(fb, &font_16, cx[0], y, COL_TXT, b);
		for (int a = 0; a < NAXES; a++) {
			fmt_pos(b, sizeof b, WD.wcs[r].off[a]);
			bool cell = f && U.wcs_col == a;
			if (cell) gfx_rect(fb, cx[a + 1] - 130, y - 3, 134, 26, COL_AMBER);
			gfx_text_r(fb, &font_16, cx[a + 1], y, cell ? COL_AMBER : COL_TXT, b);
		}
		if (r == WD.active) gfx_text(fb, &font_12, x + 700, y + 3, COL_GREEN, "ACTIVE");
		if (WD.wcs[r].job) gfx_text(fb, &font_12, x + 770, y + 3, COL_AMBER, "JOB");
		if (r < NWCS - 1) gfx_fill(fb, x + 8, y + 25, w - 16, 1, COL_EDGE);
	}
	int py = top + th + 8, ph = bot - py;
	gfx_panel(fb, x, py, 360, ph, ui_pane() == 1);
	gfx_text(fb, &font_12, x + 10, py + 6, COL_DIM, "PROBE ROUTINES  ·  SELECT RUNS");
	for (int i = 0; i < (int)ARRAY_LEN(PROBES) && 28 + i * 22 < ph - 10; i++) {
		int y = py + 26 + i * 22;
		if (ui_pane() == 1 && i == U.probe) gfx_fill(fb, x + 2, y - 2, 356, 22, RGB(0x17, 0x4a, 0x78));
		gfx_text(fb, &font_14, x + 12, y, COL_TXT, PROBES[i]);
	}
	gfx_panel(fb, x + 368, py, w - 368, ph, false);
	const char *t[] = { "Several workpieces: give each its own WCS (G54, G55 …),", "touch each off in Jog, and mark them JOB (F4).",
		"Start then runs the program on every JOB part in turn,", "with a move to safe Z between parts.",
		"Probe input: not configured on this machine." };
	for (int i = 0; i < 5; i++) gfx_text(fb, &font_14, x + 380, py + 10 + i * 21, i == 4 ? COL_AMBER : COL_DIM, t[i]);
}

void work_sk(sk_t sk[7])
{
	sk[0] = (sk_t){ "Activate", "this WCS" };
	sk[1] = (sk_t){ "From pos", "cell = here" };
	sk[2] = (sk_t){ "Edit", "cell" };
	sk[3] = (sk_t){ "In job", "toggle" };
	sk[4] = (sk_t){ "Clear", "row" };
	sk[5] = (sk_t){ "Probe", "routine" };
	sk[6] = (sk_t){ "Page R", U.shift ? "previous" : "Tools" };
}

static void set_cell(double v, int ctx) { WD.wcs[ctx / 10].off[ctx % 10] = v; workdata_save(C.wd_path); }

void work_key(pkey_t k, bool sh)
{
	if (ui_pane() == 1) {
		if (k == K_YP) U.probe = CLAMP(U.probe - 1, 0, (int)ARRAY_LEN(PROBES) - 1);
		if (k == K_YM) U.probe = CLAMP(U.probe + 1, 0, (int)ARRAY_LEN(PROBES) - 1);
		if (k == K_SEL || k == K_F6 || k == K_ENTER) act(C_PROBE, NULL, SRC_PANEL);
		return;
	}
	if (k == K_YP) { U.wcs_row = CLAMP(U.wcs_row - 1, 0, NWCS - 1); return; }
	if (k == K_YM) { U.wcs_row = CLAMP(U.wcs_row + 1, 0, NWCS - 1); return; }
	if (k == K_XP) { U.wcs_col = CLAMP(U.wcs_col + 1, 0, NAXES - 1); return; }
	if (k == K_XM) { U.wcs_col = CLAMP(U.wcs_col - 1, 0, NAXES - 1); return; }
	if (k == K_F6) { U.pane[SCR_WORK] = 1; return; }
	if (k != K_F1 && k != K_SEL && k != K_F2 && k != K_F3 && k != K_ENTER && k != K_MOD && k != K_F4 && k != K_F5) return;
	const char *w = can(C_EDIT_TABLE, NULL, SRC_PANEL);
	if (w) { ctl_toast(true, "%s", w); return; }
	wcs_t *r = &WD.wcs[U.wcs_row];
	if (k == K_F1 || k == K_SEL) { WD.active = U.wcs_row; ctl_toast(false, "G%d active", 54 + U.wcs_row); }
	else if (k == K_F2) { r->off[U.wcs_col] = C.mo.pos[U.wcs_col]; ctl_toast(false, "G%d %c = current position (work %c is now 0)", 54 + U.wcs_row, AXIS_CH[U.wcs_col], AXIS_CH[U.wcs_col]); }
	else if (k == K_F4) { r->job = !r->job; ctl_toast(false, "G%d %s the multi-part job", 54 + U.wcs_row, r->job ? "added to" : "removed from"); }
	else if (k == K_F5) { memset(r->off, 0, sizeof r->off); ctl_toast(false, "G%d cleared", 54 + U.wcs_row); }
	else {
		char l[48];
		snprintf(l, sizeof l, "G%d %c  (machine position of zero)", 54 + U.wcs_row, AXIS_CH[U.wcs_col]);
		ui_editor_open(l, r->off[U.wcs_col], SET.pos_dec, "mm", 3, 99999, true, true, set_cell, U.wcs_row * 10 + U.wcs_col);
		return;
	}
	workdata_save(C.wd_path);
}

/* ---- tools ---- */
void tools_render(fb_t *fb)
{
	int top = BODY_Y + ui_banner_h() + 8, bot = SCR_H - SK_H - 8, x = 8, w = CONTENT_W - 16;
	gfx_panel(fb, x, top, w, bot - top, true);
	char b[96];
	snprintf(b, sizeof b, "TOOL TABLE  ·  Y± TOOL  ·  X± Ø / LENGTH  ·  ENTER EDITS  ·  SIZES SHOWN AS 0.%.*s", SET.tool_dec, "000");
	gfx_text(fb, &font_12, x + 10, top + 6, COL_DIM, b);
	gfx_text(fb, &font_12, x + 14, top + 24, COL_DIM2, "TOOL");
	gfx_text_r(fb, &font_12, x + 240, top + 24, COL_DIM2, "DIAMETER");
	gfx_text_r(fb, &font_12, x + 390, top + 24, COL_DIM2, "LENGTH");
	gfx_text(fb, &font_12, x + 430, top + 24, COL_DIM2, "NOTE");
	for (int i = 0; i < NTOOLS; i++) {
		int y = top + 42 + i * 28;
		if (y > bot - 50) break;
		tool_t *t = &WD.tool[i];
		bool f = i == U.tool_row;
		if (f) gfx_fill(fb, x + 2, y - 4, w - 4, 26, RGB(0x17, 0x4a, 0x78));
		snprintf(b, sizeof b, "T%d", i + 1);
		gfx_text(fb, &font_16, x + 14, y, COL_TXT, b);
		for (int c = 0; c < 2; c++) {
			double v = c ? t->len : t->dia;
			if (v > 0) fmt_tool(b, sizeof b, v); else snprintf(b, sizeof b, "—");
			int cx = x + (c ? 390 : 240);
			if (f && U.tool_col == c) gfx_rect(fb, cx - 120, y - 3, 124, 24, COL_AMBER);
			gfx_text_r(fb, &font_16, cx, y, f && U.tool_col == c ? COL_AMBER : COL_TXT, b);
		}
		gfx_text(fb, &font_14, x + 430, y + 1, COL_DIM, t->note);
		if (i + 1 == WD.loaded_tool) gfx_text(fb, &font_12, x + 700, y + 3, COL_GREEN, "LOADED");
	}
	gfx_text(fb, &font_12, x + 12, bot - 22, COL_DIM, "F3 copies this tool's Ø to Jog › edge Ø, so a touch-off with the cutter itself lands on the edge.");
}

void tools_sk(sk_t sk[7])
{
	static char s[16];
	snprintf(s, sizeof s, "T%d", U.tool_row + 1);
	sk[0] = (sk_t){ "Load tool", s };
	sk[1] = (sk_t){ "Edit", "cell" };
	sk[2] = (sk_t){ "Edge Ø", "= this Ø" };
	sk[6] = (sk_t){ "Page R", U.shift ? "previous" : "Macros" };
}

static void set_tool(double v, int ctx) { if (ctx % 2) WD.tool[ctx / 2].len = v; else WD.tool[ctx / 2].dia = v; workdata_save(C.wd_path); }

void tools_key(pkey_t k, bool sh)
{
	tool_t *t = &WD.tool[U.tool_row];
	if (k == K_YP) U.tool_row = CLAMP(U.tool_row - 1, 0, NTOOLS - 1);
	else if (k == K_YM) U.tool_row = CLAMP(U.tool_row + 1, 0, NTOOLS - 1);
	else if (k == K_XP) U.tool_col = 1;
	else if (k == K_XM) U.tool_col = 0;
	else if (k == K_F3) { SET.edge_dia = t->dia; settings_save(C.cfg_path); char d[24]; fmt_tool(d, sizeof d, t->dia); ctl_toast(false, "Jog edge Ø = %s (T%d)", d, U.tool_row + 1); }
	else if (k == K_F1 || k == K_SEL || k == K_F2 || k == K_ENTER || k == K_MOD) {
		const char *w = can(C_EDIT_TABLE, NULL, SRC_PANEL);
		if (w) { ctl_toast(true, "%s", w); return; }
		if (k == K_F1 || k == K_SEL) { WD.loaded_tool = U.tool_row + 1; workdata_save(C.wd_path); ctl_toast(false, "T%d is the loaded tool", U.tool_row + 1); return; }
		char l[32];
		snprintf(l, sizeof l, "T%d %s", U.tool_row + 1, U.tool_col ? "length" : "diameter");
		ui_editor_open(l, U.tool_col ? t->len : t->dia, SET.tool_dec, "mm", 2, 999, false, true, set_tool, U.tool_row * 2 + U.tool_col);
	}
}
