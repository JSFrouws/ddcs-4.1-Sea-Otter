/* Files: Local (/local = the CNCDISK share) and USB sticks; Select loads. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "ui.h"

extern const char *UI_SRC_DIRS[3];
extern const char *const UI_SRC_NAMES[3];

void files_render(fb_t *fb)
{
	int top = BODY_Y + ui_banner_h() + 8, bot = SCR_H - SK_H - 8, h = bot - top;
	int x = 8, w = 330;
	gfx_panel(fb, x, top, w, h, ui_pane() == 0);
	int tx = x + 10;
	tx += gfx_text(fb, &font_12, tx, top + 6, COL_DIM, "X±  ") ;
	for (int i = 0; i < 3; i++) tx += gfx_text(fb, &font_12, tx, top + 6, i == U.src ? COL_TXT : COL_DIM2, UI_SRC_NAMES[i]) + 12;
	if (!U.nfiles) gfx_text(fb, &font_14, x + 12, top + 34, COL_DIM, "no .nc / .ngc / .tap files here");
	int first = CLAMP(U.files_idx - 8, 0, U.nfiles > 18 ? U.nfiles - 18 : 0);
	for (int i = first; i < U.nfiles && i < first + 18; i++) {
		int y = top + 28 + (i - first) * 24;
		bool f = i == U.files_idx;
		if (f) { gfx_hgrad(fb, x + 2, y - 3, w - 4, 24, RGB(0x1f, 0x5a, 0x86), COL_PANE2); gfx_fill(fb, x + 2, y - 3, 3, 24, COL_CYAN); }
		bool loaded = C.prog.loaded && !strcmp(C.prog.name, U.files[i]);
		gfx_text_fit(fb, &font_16, x + 12, y, w - 60, COL_TXT, U.files[i]);
		if (loaded) gfx_text_r(fb, &font_12, x + w - 10, y + 3, COL_GREEN, "LOADED");
	}
	int px = x + w + 8, pw = CONTENT_W - px - 8;
	gfx_panel(fb, px, top, pw, h, ui_pane() == 1);
	char b[256];
	const char *name = U.nfiles ? U.files[U.files_idx] : "";
	bool loaded = C.prog.loaded && U.nfiles && !strcmp(C.prog.name, name);
	snprintf(b, sizeof b, "PREVIEW  ·  %s", loaded ? name : C.prog.loaded ? C.prog.name : "nothing loaded");
	gfx_text_fit(fb, &font_12, px + 10, top + 6, pw - 20, COL_DIM, b);
	ui_view_render(fb, px + 2, top + 24, pw - 4, h - 140, true);
	int y = top + h - 108;
	if (U.nfiles && !loaded) {
		struct stat st;
		snprintf(b, sizeof b, "%s/%s", UI_SRC_DIRS[U.src], name);
		long sz = stat(b, &st) == 0 ? (long)st.st_size : 0;
		snprintf(b, sizeof b, "%s  ·  %ld kB  ·  Select or F1 loads it (parsed and checked first)", name, (sz + 1023) / 1024);
		gfx_text_fit(fb, &font_14, px + 12, y + 30, pw - 24, COL_AMBER, b);
		return;
	}
	if (!C.prog.loaded) return;
	char lo[3][24], hi[3][24];
	for (int a = 0; a < 3; a++) { fmt_pos(lo[a], sizeof lo[a], C.prog.lo[a]); fmt_pos(hi[a], sizeof hi[a], C.prog.hi[a]); }
	snprintf(b, sizeof b, "%d lines  ·  estimate %d:%02d:%02d", C.prog.nlines, (int)C.prog.est_s / 3600, (int)C.prog.est_s / 60 % 60, (int)C.prog.est_s % 60);
	gfx_text(fb, &font_14, px + 12, y, COL_TXT, b);
	snprintf(b, sizeof b, "X %s … %s   Y %s … %s   Z %s … %s  (work)", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
	gfx_text_fit(fb, &font_14, px + 12, y + 24, pw - 24, COL_DIM, b);
	bool inside = true;
	if (C.homed && SET.soft_limits)
		for (int a = 0; a < 3; a++) {
			double o = WD.wcs[WD.active].off[a];
			if (C.prog.lo[a] + o < SET.ax[a].soft_min || C.prog.hi[a] + o > SET.ax[a].soft_max) inside = false;
		}
	snprintf(b, sizeof b, C.homed ? (inside ? "Inside the soft limits on G%d" : "OUTSIDE the soft limits on G%d - check the work zero (Start still allowed)")
		: "Not homed - soft-limit check skipped (G%d)", 54 + WD.active);
	gfx_text_fit(fb, &font_14, px + 12, y + 48, pw - 24, inside ? COL_GREEN : COL_AMBER, b);
	if (C.prog.warn[0]) gfx_text_fit(fb, &font_12, px + 12, y + 72, pw - 24, COL_AMBER, C.prog.warn);
}

void files_sk(sk_t sk[7])
{
	sk[0] = (sk_t){ "Load", "to Run" };
	sk[1] = (sk_t){ "Simulate", "load + sim" };
	sk[2] = (sk_t){ "Source", UI_SRC_NAMES[U.src] };
	sk[3] = (sk_t){ "Rescan", "" };
}

static void load(bool sim)
{
	if (!U.nfiles) { ctl_toast(true, "No file selected"); return; }
	char path[320];
	snprintf(path, sizeof path, "%s/%s", UI_SRC_DIRS[U.src], U.files[U.files_idx]);
	arg_t a = { .path = path };
	if (act(C_LOAD, &a, SRC_PANEL) && C.prog.loaded) {
		if (sim) C.armed = ARM_SIM;
		U.screen = SCR_RUN; U.prog_line = 0;
	}
}

void files_key(pkey_t k, bool sh)
{
	if (k == K_F1 || ((k == K_SEL || k == K_ENTER) && ui_pane() == 0)) { load(false); return; }
	if (k == K_F2) { load(true); return; }
	if (k == K_F3 || (ui_pane() == 0 && (k == K_XP || k == K_XM))) { U.src = (U.src + (k == K_XM ? 2 : 1)) % 3; U.files_idx = 0; ui_scan_files(); return; }
	if (k == K_F4) { ui_scan_files(); return; }
	if (ui_pane() == 1) { ui_view_key(k); return; }
	if (k == K_YP) U.files_idx = CLAMP(U.files_idx - 1, 0, U.nfiles - 1);
	if (k == K_YM) U.files_idx = CLAMP(U.files_idx + 1, 0, U.nfiles - 1);
}
