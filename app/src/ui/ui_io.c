/* I/O overview (Settings, F6): every input, output, the pendant, the panel keys and the FPGA
 * counters, live. Inputs show what is wired to them (from the settings) and whether that
 * function is active, so a switch or a cable can be checked without a meter. */
#include <stdio.h>
#include <string.h>
#include "ui.h"

static void dot(fb_t *fb, int x, int y, bool on, uint16_t c)
{
	gfx_disc(fb, x, y, 6, on ? c : RGB(0x1c, 0x34, 0x58));
	if (!on) gfx_circle(fb, x, y, 6, COL_EDGE2);
}

static void head(fb_t *fb, int x, int y, const char *t)
{
	gfx_text(fb, &font_12, x, y, COL_CYAN, t);
}

/* what IN n (1-based) is used for, and whether that function is active right now */
static const char *in_function(int n, bool high, bool *assigned, bool *active)
{
	static char b[48];
	*assigned = true;
	for (int a = 0; a < NAXES; a++)
		if (SET.ax[a].home_port == n) { snprintf(b, sizeof b, "%c limit / home", AXIS_CH[a]); *active = high == SET.limit_level_high; return b; }
	if (SET.estop_port == n) { *active = high == SET.estop_level_high; return "E-stop"; }
	for (int a = 0; a < NAXES; a++)
		if (SET.ax[a].alarm_port == n) { snprintf(b, sizeof b, "%c drive alarm", AXIS_CH[a]); *active = !high; return b; }
	*assigned = false; *active = false;
	return "not used";
}

void io_render(fb_t *fb)
{
	int top = BODY_Y + ui_banner_h() + 8, bot = SCR_H - SK_H - 8, h = bot - top;
	char b[160];
	/* inputs */
	int x = 8, w = 440;
	gfx_panel(fb, x, top, w, h, false);
	head(fb, x + 10, top + 6, "INPUTS IN01-IN18  ·  LEVEL  ·  WHAT IT IS  ·  ACTIVE");
	int rh = (h - 30) / 18;
	for (int i = 0; i < 18; i++) {
		int y = top + 26 + i * rh;
		bool high = (C.inputs >> i) & 1, as, act;
		const char *fn = in_function(i + 1, high, &as, &act);
		if (act) gfx_fill(fb, x + 2, y - 2, w - 4, rh - 1, gfx_mix(COL_PANE2, COL_AMBER, 3));
		snprintf(b, sizeof b, "IN%02d", i + 1);
		gfx_text(fb, &font_14, x + 12, y, as ? COL_TXT : COL_DIM, b);
		dot(fb, x + 72, y + 8, high, COL_CYAN);
		gfx_text(fb, &font_12, x + 84, y + 2, COL_DIM, high ? "high" : "low");
		gfx_text(fb, &font_14, x + 130, y, as ? COL_TXT : COL_DIM2, fn);
		if (as) gfx_text_r(fb, &font_14, x + w - 12, y, act ? COL_AMBER : COL_GREEN, act ? "ACTIVE" : "ok");
	}

	/* right column: outputs, pendant, panel keys, FPGA */
	int rx = x + w + 8, rw = CONTENT_W - rx - 8, y = top;
	int oh = 104;
	gfx_panel(fb, rx, y, rw, oh, false);
	head(fb, rx + 10, y + 6, "OUTPUTS");
	const char *ofn[3] = { "", "", "" };
	if (SET.out_spindle >= 1 && SET.out_spindle <= 3) ofn[SET.out_spindle - 1] = "spindle (M3)";
	if (SET.out_flood >= 1 && SET.out_flood <= 3) ofn[SET.out_flood - 1] = "coolant (M8)";
	if (SET.out_aux >= 1 && SET.out_aux <= 3) ofn[SET.out_aux - 1] = "aux (M10)";
	if (SET.out_enable >= 1 && SET.out_enable <= 3) ofn[SET.out_enable - 1] = SET.enable_level ? "drive enable (high = on)" : "drive enable (low = on)";
	for (int o = 0; o < 3; o++) {
		int yy = y + 26 + o * 20;
		bool on = (C.fp.outputs >> o) & 1;
		snprintf(b, sizeof b, "OUT%d", o + 1);
		gfx_text(fb, &font_14, rx + 12, yy, COL_TXT, b);
		dot(fb, rx + 66, yy + 8, on, COL_GREEN);
		gfx_text(fb, &font_14, rx + 82, yy, ofn[o][0] ? COL_TXT : COL_DIM2, ofn[o][0] ? ofn[o] : "not used");
	}
	double rpm = C.spindle ? C.rpm * C.mo.spin_ov / 100.0 : 0;
	snprintf(b, sizeof b, "Spindle 0-10 V: %.0f rpm = %.2f V", rpm, SET.spindle_max > 0 ? 10.0 * rpm / SET.spindle_max : 0);
	gfx_text(fb, &font_14, rx + 12, y + 84, COL_DIM, b);

	y += oh + 8;
	int ph = 136;
	gfx_panel(fb, rx, y, rw, ph, false);
	head(fb, rx + 10, y + 6, "PENDANT (MPG)");
	uint16_t pr = C.pend.raw;
	bool dead = (pr & 0x3FF) == 0x3FF;                  /* every line high: no pendant / cable / power */
	snprintf(b, sizeof b, "raw 0x%03X", pr);
	gfx_text_r(fb, &font_12, rx + rw - 12, y + 6, COL_DIM2, b);
	const char *pl[] = { "E-stop", "Dead-man (hold)", "Axis switch", "Multiplier", "Wheel" };
	char pv[5][48];
	snprintf(pv[0], sizeof pv[0], "%s", C.pend.estop ? (SET.pendant_estop ? "PRESSED" : "pressed (ignored, Settings)") : "released");
	snprintf(pv[1], sizeof pv[1], "%s", C.pend.held ? "held" : "released");
	snprintf(pv[2], sizeof pv[2], "%s", !C.pend.held ? "(read only while held)" : C.pend.axis < 0 ? "OFF" : (const char *[]){ "X", "Y", "Z", "4" }[C.pend.axis]);
	snprintf(pv[3], sizeof pv[3], "%s", C.pend.held ? (C.pend.mult == 100 ? "×100" : C.pend.mult == 10 ? "×10" : "×1") : "(read only while held)");
	snprintf(pv[4], sizeof pv[4], "%d counts", (int)U.wheel_total);
	for (int i = 0; i < 5; i++) {
		int yy = y + 24 + i * 18;
		gfx_text(fb, &font_14, rx + 12, yy, COL_DIM, pl[i]);
		uint16_t c = i == 0 && C.pend.estop ? COL_RED : (i == 1 && C.pend.held) ? COL_AMBER : COL_TXT;
		gfx_text(fb, &font_14, rx + 160, yy, c, pv[i]);
	}
	if (dead) gfx_text_fit(fb, &font_12, rx + 12, y + ph - 22, rw - 24, COL_AMBER, "All lines high: unplugged, loose cable or no power?");

	y += ph + 8;
	int kh = 64;
	gfx_panel(fb, rx, y, rw, kh, false);
	head(fb, rx + 10, y + 6, "PANEL KEYS HELD  ·  LEDS");
	char kb[160] = "";
	for (int k = 1; k < K__N; k++)
		if ((U.keys_raw >> k) & 1) snprintf(kb + strlen(kb), sizeof kb - strlen(kb), "%s ", KEY_NAMES[k]);
	gfx_text_fit(fb, &font_14, rx + 12, y + 22, rw - 24, kb[0] ? COL_AMBER : COL_DIM2, kb[0] ? kb : "none");
	uint8_t l = ui_leds();
	snprintf(b, sizeof b, "Start %s  ·  Pause %s  ·  Reset %s  ·  Spindle %s  ·  Shift %s", l & LED_START ? "on" : "off", l & LED_PAUSE ? "on" : "off",
		l & LED_RESET ? "on" : "off", l & LED_SPINDLE ? "on" : "off", l & LED_SHIFT ? "on" : "off");
	gfx_text_fit(fb, &font_12, rx + 12, y + 42, rw - 24, COL_DIM, b);

	y += kh + 8;
	gfx_panel(fb, rx, y, rw, bot - y, false);
	head(fb, rx + 10, y + 6, "FPGA");
	snprintf(b, sizeof b, "version 0x%04X  ·  %s", fpga_version(&C.fp), C.sim ? "SIMULATED machine" : "real");
	gfx_text(fb, &font_12, rx + 12, y + 22, COL_DIM, b);
	for (int a = 0; a < NAXES; a++) {
		snprintf(b, sizeof b, "%c  counter %d  ·  commanded %lld", AXIS_CH[a], C.fpos[a], (long long)C.mo.steps[a]);
		gfx_text(fb, &font_12, rx + 12, y + 38 + a * 15, C.fpos[a] == (int32_t)C.mo.steps[a] ? COL_TXT : COL_AMBER, b);
	}
	snprintf(b, sizeof b, "position checks %u  ·  faults %u", C.pos_checks, C.pos_faults);
	gfx_text(fb, &font_12, rx + 12, y + 38 + 4 * 15, C.pos_faults ? COL_AMBER : COL_DIM, b);
}

void io_sk(sk_t sk[7])
{
	sk[5] = (sk_t){ "Back", "settings" };
}

void io_key(pkey_t k, bool sh)
{
	if (k == K_F6 || k == K_CANCEL) ui_goto(SCR_SETTINGS);
}
