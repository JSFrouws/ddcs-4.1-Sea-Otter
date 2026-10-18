/* Macros: preset moves and commands instead of MDI typing (no keyboard on the panel).
 * Y± picks, Select / Enter / F1 runs. Positions live in Settings > Macros. */
#include <stdio.h>
#include <string.h>
#include "ui.h"

#define MAC_ZEROALL MAC__N                          /* UI-only: a table edit, not a move */
#define NITEMS (MAC__N + 1)

static void describe(int i, char *b, size_t n)
{
	char x[24], y[24], z[24];
	const double *p = i == MAC_PARK ? SET.park : SET.tc;
	fmt_pos(x, sizeof x, p[0]); fmt_pos(y, sizeof y, p[1]); fmt_pos(z, sizeof z, p[2]);
	char s[24];
	fmt_pos(s, sizeof s, SET.safe_z);
	switch (i) {
	case MAC_SAFEZ: snprintf(b, n, "Z up to %s mm above work zero (never down)", s); break;
	case MAC_WORKZERO: snprintf(b, n, "Z up to safe height, then X Y to G%d zero", 54 + WD.active); break;
	case MAC_PARK: case MAC_TOOLCHANGE: snprintf(b, n, "Z up, X Y to %s / %s, Z to %s (machine)", x, y, z); break;
	case MAC_WARMUP: snprintf(b, n, "Spindle %.0f rpm for %.0f s, then off", SET.warm_rpm, SET.warm_s); break;
	default: snprintf(b, n, "G%d X Y Z zero = the current position", 54 + WD.active); break;
	}
}

void macros_render(fb_t *fb)
{
	int top = BODY_Y + ui_banner_h() + 8, bot = SCR_H - SK_H - 8, x = 8, w = CONTENT_W - 16;
	gfx_panel(fb, x, top, w, bot - top, true);
	gfx_text(fb, &font_12, x + 10, top + 6, COL_DIM, "MACROS  ·  Y± PICK  ·  SELECT / ENTER RUNS  ·  POSITIONS IN SETTINGS › MACROS");
	for (int i = 0; i < NITEMS; i++) {
		int y = top + 30 + i * 44;
		if (i == U.macro) gfx_fill(fb, x + 2, y - 4, w - 4, 40, RGB(0x17, 0x4a, 0x78));
		const char *name = i == MAC_ZEROALL ? "Zero all" : MACRO_NAMES[i];
		gfx_text(fb, &font_16, x + 14, y, COL_TXT, name);
		char b[160];
		describe(i, b, sizeof b);
		gfx_text(fb, &font_12, x + 14, y + 20, COL_DIM, b);
		bool homed_only = (i == MAC_PARK || i == MAC_TOOLCHANGE) && !C.homed;
		if (homed_only) gfx_text_r(fb, &font_12, x + w - 14, y + 3, COL_AMBER, "home first");
	}
}

void macros_sk(sk_t sk[7])
{
	sk[0] = (sk_t){ "Run", U.macro == MAC_ZEROALL ? "set zero" : "macro" };
	sk[6] = (sk_t){ "Page R", U.shift ? "previous" : "Settings" };
}

void macros_key(pkey_t k, bool sh)
{
	if (k == K_YP) { U.macro = CLAMP(U.macro - 1, 0, NITEMS - 1); return; }
	if (k == K_YM) { U.macro = CLAMP(U.macro + 1, 0, NITEMS - 1); return; }
	if (k != K_F1 && k != K_SEL && k != K_ENTER) return;
	if (U.macro == MAC_ZEROALL) {
		const char *why = can(C_EDIT_TABLE, NULL, SRC_PANEL);
		if (why) { ctl_toast(true, "%s", why); return; }
		for (int a = 0; a <= AX_Z; a++) WD.wcs[WD.active].off[a] = C.mo.pos[a];
		workdata_save(C.wd_path);
		ctl_toast(false, "G%d X Y Z zero = here", 54 + WD.active);
		return;
	}
	arg_t a = { .kind = U.macro };
	act(C_MACRO, &a, SRC_PANEL);
}
