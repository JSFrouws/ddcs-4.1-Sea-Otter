#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "ui.h"

ui_t U;
void (*ui_yield)(void);
const char *const KEY_NAMES[K__N] = { "", "F1", "F2", "F3", "F4", "F5", "F6", "F7",
	"TAB", "SEL", "SHIFT", "MOD", "MODE", "CANCEL", "YP", "ENTER", "XM", "YM", "XP" };
const double JOG_STEPS[] = { 0.01, 0.1, 1, 10, 0 };   /* 0 = continuous */
const int NJOG_STEPS = 5;
const char *UI_SRC_DIRS[3] = { "/local", "/udisk-sda1", "/udisk-sda" };
const char *const UI_SRC_NAMES[3] = { "Local", "USB", "USB (2)" };

static const scr_t PAGES[] = { SCR_RUN, SCR_FILES, SCR_WORK, SCR_TOOLS, SCR_MACROS, SCR_SETTINGS };
static const char *const PANES[SCR__N][2] = {
	{ "Toolpath", "Program" }, { "Jog keys", "Jog setup" }, { "File list", "Preview" },
	{ "Offsets", "Probe" }, { "Tool table", NULL }, { "Groups", "Values" }, { "Macros", NULL } };

pkey_t key_by_name(const char *s)
{
	for (int i = 1; i < K__N; i++) if (!strcasecmp(s, KEY_NAMES[i])) return (pkey_t)i;
	return K_NONE;
}

int ui_npanes(scr_t s) { return PANES[s][1] ? 2 : 1; }
const char *ui_pane_name(scr_t s, int i) { return PANES[s][i % ui_npanes(s)]; }
int ui_pane(void) { return U.pane[U.screen] % ui_npanes(U.screen); }

void fmt_pos(char *b, size_t n, double v)
{
	double r = v; int d = SET.pos_dec;
	if (fabs(r) < 0.5 * pow(10, -d)) r = 0;
	snprintf(b, n, "%s%.*f", r < 0 ? "−" : "", d, fabs(r));
}

void fmt_tool(char *b, size_t n, double v) { snprintf(b, n, "%.*f", SET.tool_dec, v); }

void ui_init(void)
{
	memset(&U, 0, sizeof U);
	U.view.iso = SET.view_iso == 1; U.view.saver = SET.view_iso == 2; U.view.yaw = -30; U.view.pitch = 32; U.view.zoom = 1;
	U.step = 2; U.tool_row = 2; U.dirty = true;
	ui_scan_files();
}

static int has_ext(const char *n)
{
	const char *e = strrchr(n, '.');
	return e && (!strcasecmp(e, ".nc") || !strcasecmp(e, ".ngc") || !strcasecmp(e, ".tap") || !strcasecmp(e, ".gcode") || !strcasecmp(e, ".txt"));
}

static void add_file(const char *name, void *ctx)
{
	(void)ctx;
	if (U.nfiles < MAXFILES && name[0] != '.' && has_ext(name)) snprintf(U.files[U.nfiles++], sizeof U.files[0], "%.159s", name);
}

void ui_scan_files(void)
{
	U.nfiles = 0;
	dir_list(UI_SRC_DIRS[U.src], add_file, NULL);
	for (int i = 1; i < U.nfiles; i++)                  /* sort by name */
		for (int j = i; j > 0 && strcasecmp(U.files[j - 1], U.files[j]) > 0; j--) {
			char t[160]; memcpy(t, U.files[j], sizeof t); memcpy(U.files[j], U.files[j - 1], sizeof t); memcpy(U.files[j - 1], t, sizeof t);
		}
	if (U.files_idx >= U.nfiles) U.files_idx = 0;
}

static void page(int d)
{
	int i = 0, n = (int)ARRAY_LEN(PAGES);
	for (int k = 0; k < n; k++) if (PAGES[k] == U.screen) i = k;
	U.screen = PAGES[(i + d + n) % n];
	if (U.screen == SCR_FILES) ui_scan_files();
	if (U.screen == SCR_MACROS) { extern void macros_scan(void); macros_scan(); }
}

static void toggle_mode(void)
{
	if (U.screen == SCR_JOG) { U.screen = SCR_RUN; return; }
	if (ctl_busy()) { ctl_toast(true, "Jog is locked while %s - Pause / Reset first", STATE_NAMES[C.m]); return; }
	U.screen = SCR_JOG;
}

void ui_key(pkey_t k)
{
	U.dirty = true;
	bool sh = U.shift;
	if (U.ed.on) {
		if (k == K_SHIFT) U.shift = !U.shift;
		else { U.shift = false; ui_editor_key(k, sh); }
		return;
	}
	if (k == K_SHIFT) { U.shift = !U.shift; C.touch_axis = -1; return; }
	U.shift = false;
	if (!(sh && U.screen == SCR_JOG)) C.touch_axis = -1;
	bool busy = ctl_busy() || (!motion_idle(&C.mo) && C.m == M_IDLE);   /* a jog in progress counts */
	if (k == K_F3 && C.m == M_OFF) act(C_DRIVES_ON, NULL, SRC_PANEL);          /* any screen */
	else if (k == K_F3 && C.m == M_ESTOP) ctl_toast(true, "Release the E-stop first (%s)", C.stop_cause[0] ? C.stop_cause : "E-stop input active");
	else if (k == K_F2 && busy) act(C_PAUSE, NULL, SRC_PANEL);
	else if (k == K_F3 && C.countdown) { C.countdown = 0; ctl_toast(false, "Remote start cancelled"); }
	else if (k == K_F3 && (busy || C.m == M_LIMIT || C.macro_on)) act(C_STOP, NULL, SRC_PANEL);
	else if (k == K_F7) page(sh ? -1 : 1);
	else if (k == K_MODE) toggle_mode();
	else if (k == K_TAB) { int n = ui_npanes(U.screen); U.pane[U.screen] = (U.pane[U.screen] + (sh ? n - 1 : 1)) % n; }
	else switch (U.screen) {
	case SCR_RUN: run_key(k, sh); break;
	case SCR_JOG: jog_key(k, sh); break;
	case SCR_FILES: files_key(k, sh); break;
	case SCR_WORK: work_key(k, sh); break;
	case SCR_TOOLS: tools_key(k, sh); break;
	case SCR_SETTINGS: set_key(k, sh); break;
	case SCR_MACROS: macros_key(k, sh); break;
	default: break;
	}
}

void jog_key_release(pkey_t k);
void ui_key_release(pkey_t k) { jog_key_release(k); U.dirty = true; }

void ui_render(fb_t *fb)
{
	sk_t sk[7];
	memset(sk, 0, sizeof sk);
	gfx_noclip(fb);
	gfx_vgrad(fb, 0, 0, SCR_W, 140, COL_BG0, COL_BG1);
	gfx_fill(fb, 0, 140, SCR_W, SCR_H - 140, COL_BG1);
	if (C.m == M_RUN && U.screen == SCR_JOG) U.screen = SCR_RUN;   /* started remotely while on Jog */
	/* CPU saver ends on an E-stop or a limit: the operator needs to see where the tool is
	 * (user requirement). It stays on ISO until changed again with F4. */
	static mstate_t last_m;
	if (C.m != last_m && (C.m == M_ESTOP || C.m == M_LIMIT) && U.view.saver) {
		U.view.saver = false; U.view.iso = true;
		ctl_toast(false, "CPU saver off - showing the isometric view");
	}
	last_m = C.m;
	gfx_clip(fb, 0, BODY_Y + ui_banner_h(), CONTENT_W, SCR_H - SK_H);
	switch (U.screen) {
	case SCR_RUN: run_render(fb); run_sk(sk); break;
	case SCR_JOG: jog_render(fb); jog_sk(sk); break;
	case SCR_FILES: files_render(fb); files_sk(sk); break;
	case SCR_WORK: work_render(fb); work_sk(sk); break;
	case SCR_TOOLS: tools_render(fb); tools_sk(sk); break;
	case SCR_SETTINGS: set_render(fb); set_sk(sk); break;
	case SCR_MACROS: macros_render(fb); macros_sk(sk); break;
	default: break;
	}
	gfx_noclip(fb);
	if (ctl_busy()) {                       /* while anything moves, F2/F3 are Pause/Reset everywhere */
		sk[1] = (sk_t){ "Pause", C.m == M_RUN ? "feed hold" : "stop" };
		sk[2] = (sk_t){ "Reset", "stop" };
		if (U.screen != SCR_RUN) sk[0] = (sk_t){ NULL, NULL };
	}
	if (C.m == M_OFF) sk[2] = (sk_t){ "Reset", "drives ON" };
	if (C.m == M_ESTOP) sk[2] = (sk_t){ "Reset", "release E-stop" };
	ui_frame(fb, sk);
	ui_editor_render(fb);
	U.dirty = false;
}

uint8_t ui_leds(void)
{
	bool blink = (now_ms() / 350) & 1;
	uint8_t l = 0;
	if (C.m == M_RUN && (C.run_mode == ARM_RUN || blink)) l |= LED_START;
	if (C.m == M_HOLD) l |= LED_PAUSE;
	if (C.m == M_ESTOP || (C.m == M_LIMIT && blink)) l |= LED_RESET;
	if (C.spindle) l |= LED_SPINDLE;
	if (U.shift) l |= LED_SHIFT;
	if (C.agent.on) l |= LED_ETH;
	return l;
}
