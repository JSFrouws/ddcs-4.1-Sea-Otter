/* Macros: the .nc files in the macro folders (core/macro.h) - nothing is built in. Y± picks,
 * Select / Enter / F1 arms the macro and goes to the Run page, where Start runs it, Pause
 * holds it and Reset stops it; the loaded program comes back afterwards. */
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "../core/macro.h"

static macro_t list[MACRO_MAX];
static int nlist = -1;

void macros_scan(void) { nlist = macro_scan(list, MACRO_MAX); if (U.macro >= nlist) U.macro = nlist > 0 ? nlist - 1 : 0; }

void macros_render(fb_t *fb)
{
	if (nlist < 0) macros_scan();
	int top = BODY_Y + ui_banner_h() + 8, bot = SCR_H - SK_H - 8, x = 8, w = CONTENT_W - 16;
	gfx_panel(fb, x, top, w, bot - top, true);
	gfx_text(fb, &font_12, x + 10, top + 6, COL_DIM, "MACROS  ·  Y± PICK  ·  SELECT / ENTER ARMS IT ON THE RUN PAGE  ·  F4 RESCAN");
	int rows = (bot - top - 110) / 40;
	int first = CLAMP(U.macro - rows + 1, 0, nlist > rows ? nlist - rows : 0);
	for (int i = first; i < nlist && i < first + rows; i++) {
		int y = top + 28 + (i - first) * 40;
		if (i == U.macro) gfx_fill(fb, x + 2, y - 4, w - 4, 38, RGB(0x17, 0x4a, 0x78));
		gfx_text(fb, &font_16, x + 14, y, COL_TXT, list[i].name);
		gfx_text_fit(fb, &font_12, x + 14, y + 19, w - 220, COL_DIM, list[i].info[0] ? list[i].info : "(no info line)");
		const char *tag = list[i].needs_homed && !C.homed ? "home first" : list[i].user ? "network share" : "package";
		gfx_text_r(fb, &font_12, x + w - 14, y + 3, list[i].needs_homed && !C.homed ? COL_AMBER : COL_DIM2, tag);
	}
	if (nlist == 0) gfx_text(fb, &font_16, x + 14, top + 32, COL_AMBER, "No macros found - see below how to add them");
	/* how to add your own */
	int hy = bot - 92;
	gfx_coral(fb, x + 8, hy - 12, w - 16);
	char b[256];
	snprintf(b, sizeof b, "Your own macros: put .nc files in %.120s (on the controller: the CNCDISK network share, folder \"macros\").",
		macro_dir_user[0] ? macro_dir_user : "<programs>/macros");
	gfx_text_fit(fb, &font_12, x + 14, hy, w - 28, COL_TXT, b);
	gfx_text_fit(fb, &font_12, x + 14, hy + 18, w - 28, COL_DIM,
		"Plain G-code. Optional first lines: (name: ...)  (info: ...)  (needs: homed). Values filled in at Start: {jog.safe_z} or any setting key,");
	gfx_text_fit(fb, &font_12, x + 14, hy + 34, w - 28, COL_DIM,
		"{machine.x} {work.z} (position), {max:work.z,jog.safe_z}. The package's own macro-*.nc files live in the app folder and come with each update.");
	gfx_text_fit(fb, &font_12, x + 14, hy + 50, w - 28, COL_DIM2, "Details and examples: docs/user-manual.md, section Macros.");
}

void macros_sk(sk_t sk[7])
{
	sk[0] = (sk_t){ "Arm", "→ Run page" };
	sk[3] = (sk_t){ "Rescan", "folders" };
	sk[6] = (sk_t){ "Page R", U.shift ? "previous" : "Settings" };
}

void macros_key(pkey_t k, bool sh)
{
	if (nlist < 0) macros_scan();
	if (k == K_YP) { U.macro = CLAMP(U.macro - 1, 0, nlist > 0 ? nlist - 1 : 0); return; }
	if (k == K_YM) { U.macro = CLAMP(U.macro + 1, 0, nlist > 0 ? nlist - 1 : 0); return; }
	if (k == K_F4) { macros_scan(); ctl_toast(false, "%d macros", nlist); return; }
	if (k != K_F1 && k != K_SEL && k != K_ENTER) return;
	if (U.macro < 0 || U.macro >= nlist) return;
	arg_t a = { .path = list[U.macro].path };
	if (act(C_MACRO, &a, SRC_PANEL) && C.macro_on) U.screen = SCR_RUN;   /* armed: Start runs it there */
}
