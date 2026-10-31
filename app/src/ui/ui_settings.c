/* Settings: groups on the left, values on the right, all from the SETTINGS[] table.
 * "I/O & diagnostics" shows the live inputs, pendant bits, outputs and FPGA version. */
#include <stdio.h>
#include <string.h>
#include "ui.h"

/* Inline edit (on/off, choice lists, integers < 100): same flow as the digit editor -
 * one quick press / Modify / Enter starts it, Y± X± (or hold + wheel) change the value,
 * Enter / quick press accepts, Cancel discards. Nothing changes until it is started. */
static struct { bool on; const setting_desc_t *d; double v; } inl;
bool set_inline_active(void) { return inl.on && U.screen == SCR_SETTINGS; }
static bool is_inline(const setting_desc_t *d) { return d->type == ST_BOOL || d->type == ST_ENUM || d->type == ST_PORT || (d->type == ST_INT && d->max < 100); }
static void inline_step(int n)
{
	const setting_desc_t *d = inl.d;
	if (d->type == ST_BOOL) { inl.v = !inl.v; return; }
	inl.v += n;
	if (d->type == ST_ENUM || d->type == ST_PORT) { if (inl.v > d->max) inl.v = d->min; if (inl.v < d->min) inl.v = d->max; }
	else inl.v = CLAMP(inl.v, d->min, d->max);
}

/* A setting that does nothing because of another one is shown greyed and can't be edited
 * (user: "disable the options that are not applicable"). Returns why, or NULL. */
static const char *inapplicable(const setting_desc_t *d)
{
	const char *k = d->key;
	if (!strncmp(k, "api.", 4) && strcmp(k, "api.on") && !SET.api) return "API is off";
	if (!strcmp(k, "api.start_delay") && !SET.remote_start) return "START is off";
	if (!strncmp(k, "pendant.", 8) && strcmp(k, "pendant.on") && !SET.pendant) return "no pendant fitted";
	if (!strcmp(k, "estop.level_high") && !SET.estop_port) return "no E-stop input";
	if (!strcmp(k, "drives.enable_on") && !SET.out_enable) return "no enable output";
	if (!strncmp(k, "axis.", 5) && strstr(k, ".soft_m") && !SET.soft_limits) return "soft limits are off";
	if (!strcmp(k, "macro.warm_s") && SET.warm_rpm <= 0) return "warm-up speed is 0";
	return NULL;
}

static bool show_hint;                             /* F5: the hint for the focused setting */

/* text wrapped into lines of at most w pixels; returns the number of lines drawn */
static int wrap(fb_t *fb, const font_t *f, int x, int y, int w, int lh, uint16_t c, const char *s, int maxl)
{
	char line[200];
	int n = 0;
	while (*s && n < maxl) {
		size_t len = 0, best = 0;
		while (s[len]) {
			size_t e = len;
			while (s[e] && s[e] != ' ') e++;
			if (e >= sizeof line) break;
			memcpy(line, s, e); line[e] = 0;
			if (gfx_text_w(f, line) > w && best) break;
			best = e;
			if (!s[e]) break;
			len = e + 1;
		}
		if (!best) best = strlen(s) < sizeof line - 1 ? strlen(s) : sizeof line - 1;
		memcpy(line, s, best); line[best] = 0;
		if (fb) gfx_text(fb, f, x, y + n * lh, c, line);
		n++;
		s += best;
		while (*s == ' ') s++;
	}
	return n;
}

/* top right; at the bottom right instead when the focused row (fy) would be under it */
static void hint_box(fb_t *fb, int vx, int vw, int top, int low, int fy, const setting_desc_t *d)
{
	const char *h = d ? setting_hint(d->key) : "Pick a setting on the right: F5 shows what it does.";
	if (!h) return;
	int w = 380, x = vx + vw - w - 8, y = top + 24;
	int n = wrap(NULL, &font_14, 0, 0, w - 24, 18, 0, h, 8);
	int bh = 34 + n * 18 + 8;
	if (fy >= 0 && fy + 26 > y && fy < y + bh) y = low - bh - 4;
	gfx_fill(fb, x, y, w, bh, RGB(0x05, 0x14, 0x2b));
	gfx_rect(fb, x, y, w, bh, COL_CYAN);
	gfx_text_fit(fb, &font_12, x + 12, y + 8, w - 24, COL_CYAN, d ? d->label : "Hints");
	wrap(fb, &font_14, x + 12, y + 30, w - 24, 18, COL_TXT, h, 8);
}

static int ngroups(void) { int n = 0; while (SETTING_GROUPS[n]) n++; return n; }

/* settings of the current group, by index */
static int group_items(const setting_desc_t **out, int max)
{
	int n = 0;
	for (int i = 0; i < NSETTINGS && n < max; i++)
		if (!strcmp(SETTINGS[i].group, SETTING_GROUPS[U.grp])) out[n++] = &SETTINGS[i];
	return n;
}

void set_render(fb_t *fb)
{
	int top = BODY_Y + ui_banner_h() + 8, bot = SCR_H - SK_H - 8, h = bot - top;
	int x = 8, w = 220;
	gfx_panel(fb, x, top, w, h, ui_pane() == 0);
	gfx_text(fb, &font_12, x + 10, top + 6, COL_DIM, "GROUP");
	const setting_desc_t *items[64];
	int nitems = group_items(items, 64);
	bool back = ui_pane() == 1 && (U.item < 0 || U.item >= nitems);   /* cursor on "back to the groups" */
	for (int i = 0; SETTING_GROUPS[i]; i++) {
		int y = top + 28 + i * 28;
		if (i == U.grp) { gfx_hgrad(fb, x + 2, y - 4, w - 4, 26, RGB(0x1f, 0x5a, 0x86), COL_PANE2); gfx_fill(fb, x + 2, y - 4, 3, 26, back ? COL_AMBER : COL_CYAN); }
		int tw = gfx_text(fb, &font_16, x + 12, y, COL_TXT, SETTING_GROUPS[i]);
		if (i == U.grp && back) {                       /* "Axes  <━━" in orange: Enter goes back here */
			int ax = x + 12 + tw + 10, ay = y + 9;
			for (int k = 0; k < 9; k++) gfx_fill(fb, ax + k, ay - k, 2, 2 * k + 1, COL_AMBER);   /* arrow head */
			gfx_fill(fb, ax + 8, ay - 3, 34, 7, COL_AMBER);                                       /* shaft */
			gfx_rect(fb, x + 2, y - 4, w - 4, 26, COL_AMBER);
		}
	}
	int vx = x + w + 8, vw = CONTENT_W - vx - 8;
	gfx_panel(fb, vx, top, vw, h, ui_pane() == 1);
	char hd[96];
	snprintf(hd, sizeof hd, inl.on ? "%s  ·  EDITING: Y± / X± / HOLD + WHEEL CHANGE  ·  ENTER OR QUICK PRESS ACCEPTS  ·  CANCEL DISCARDS"
		: "%s  ·  QUICK PRESS / MODIFY / ENTER STARTS EDITING  ·  CANCEL BACK", SETTING_GROUPS[U.grp]);
	gfx_text(fb, &font_12, vx + 10, top + 6, COL_DIM, hd);
	if (!strcmp(SETTING_GROUPS[U.grp], "I/O & diagnostics")) {
		gfx_text(fb, &font_16, vx + 12, top + 34, COL_TXT, "F6  I/O  opens the live overview of every input and output,");
		gfx_text(fb, &font_16, vx + 12, top + 58, COL_TXT, "the pendant, the panel keys and the FPGA counters.");
		if (back) gfx_text(fb, &font_16, vx + 12, bot - 50, COL_AMBER, "←  back to the groups  ·  Enter / quick press");
		return;
	}
	const setting_desc_t **it = items;
	bool note = !strcmp(SETTING_GROUPS[U.grp], "API");
	int foot = 30 + (note ? 38 : 0);                   /* the notes at the bottom keep their own room */
	int fy = -1;                                        /* y of the focused row (the hint box avoids it) */
	int n = nitems, rows = (h - 40 - foot) / 26 - 1;   /* one row kept for the "back" line */
	if (back && U.item < 0) {                           /* before the first: the back line on top */
		int y = top + 28;
		gfx_fill(fb, vx + 2, y - 4, vw - 4, 25, RGB(0x3a, 0x2c, 0x10));
		gfx_rect(fb, vx + 2, y - 4, vw - 4, 25, COL_AMBER);
		gfx_text(fb, &font_16, vx + 12, y - 1, COL_AMBER, "←  back to the groups  ·  Enter / quick press");
		top += 26;
	}
	int first = CLAMP(U.item - rows + 2, 0, n > rows ? n - rows : 0);
	for (int i = first; i < n && i < first + rows; i++) {
		int y = top + 28 + (i - first) * 26;
		bool f = ui_pane() == 1 && i == U.item;
		if (f) fy = y - 4;
		if (f) { gfx_hgrad(fb, vx + 2, y - 4, vw - 4, 25, RGB(0x1f, 0x5a, 0x86), COL_PANE2); gfx_fill(fb, vx + 2, y - 4, 3, 25, COL_CYAN); }
		char v[64];
		bool editing = f && inl.on && inl.d == it[i];
		if (editing) {                                  /* show the value being edited */
			double keep = setting_get(it[i]);
			setting_set(it[i], inl.v); setting_format(it[i], v, sizeof v); setting_set(it[i], keep);
			char w[80]; snprintf(w, sizeof w, "◀  %s  ▶", v); snprintf(v, sizeof v, "%.60s", w);
			gfx_fill(fb, vx + vw - 230, y - 4, 226, 25, RGB(0x3a, 0x2c, 0x10));
			gfx_rect(fb, vx + vw - 230, y - 4, 226, 25, COL_AMBER);
			gfx_text_r(fb, &font_16, vx + vw - 12, y - 1, COL_AMBER, v);
			gfx_text_fit(fb, &font_14, vx + 12, y, vw - 250, COL_TXT, it[i]->label);
			continue;
		}
		setting_format(it[i], v, sizeof v);
		const char *na = inapplicable(it[i]);
		bool master = !strcmp(it[i]->key, "api.on");      /* the switch everything else depends on */
		if (master) gfx_fill(fb, vx + 8, y + 22, vw - 16, 1, COL_CYAN);
		gfx_text_fit(fb, master ? &font_16 : &font_14, vx + 12, y - (master ? 1 : 0), vw - 230, na ? COL_DIM2 : master ? COL_CYAN : COL_TXT, it[i]->label);
		if (na) gfx_text_r(fb, &font_12, vx + vw - 110, y + 2, COL_DIM2, na);
		else if (it[i]->machine) gfx_text(fb, &font_12, vx + vw - 210, y + 2, COL_DIM2, "idle only");
		uint16_t vc = na ? COL_DIM2 : it[i]->type == ST_BOOL ? (setting_get(it[i]) ? COL_GREEN : COL_DIM) : COL_TXT;
		gfx_text_r(fb, &font_16, vx + vw - 12, y - 1, vc, v);
	}
	if (back && U.item >= n) {                          /* past the last: the back line below it */
		int y = top + 28 + (n - first < rows ? n - first : rows) * 26;
		gfx_fill(fb, vx + 2, y - 4, vw - 4, 25, RGB(0x3a, 0x2c, 0x10));
		gfx_rect(fb, vx + 2, y - 4, vw - 4, 25, COL_AMBER);
		gfx_text(fb, &font_16, vx + 12, y - 1, COL_AMBER, "←  back to the groups  ·  Enter / quick press");
	}
	if (show_hint) hint_box(fb, vx, vw, top, bot - foot, fy, ui_pane() == 1 && U.item >= 0 && U.item < nitems ? items[U.item] : NULL);
	if (note)
		wrap(fb, &font_12, vx + 12, bot - 60, vw - 24, 16, SET.api ? COL_DIM : COL_AMBER, SET.api
			? "Reading (status, offsets, tools, programs) and Pause / Stop are always allowed. These switches can only be changed here, never over the API."
			: "The API is OFF: nothing on the network can read or control this machine. Switch it on above, then pick what it may do.", 2);
	gfx_text_fit(fb, &font_12, vx + 12, bot - 24, vw - 24, COL_DIM2, "Changes apply at once and are saved; F2 restores this machine's original values.");
}

void set_sk(sk_t sk[7])
{
	sk[0] = (sk_t){ "Save", "" };
	sk[1] = (sk_t){ "Defaults", "machine" };
	sk[2] = (sk_t){ "Edit", inl.on ? "accept" : "value" };
	sk[3] = (sk_t){ "Export", "to USB" };
	sk[4] = (sk_t){ "Hint", show_hint ? "hide" : "show" };
	sk[5] = (sk_t){ "I/O", "live overview" };
}

static void set_num(double v, int ctx)
{
	setting_set(&SETTINGS[ctx], v);
	settings_save(C.cfg_path);
	if (!strncmp(SETTINGS[ctx].key, "axis.", 5) && !ctl_busy() && motion_idle(&C.mo)) ctl_apply_axis_config();
}

static void changed(const setting_desc_t *d)
{
	settings_save(C.cfg_path);
	if (!strcmp(d->key, "display.view")) { U.view.iso = SET.view_iso == 1; U.view.saver = SET.view_iso == 2; }
	if (!strncmp(d->key, "axis.", 5) && !ctl_busy() && motion_idle(&C.mo)) ctl_apply_axis_config();
	if (!strcmp(d->key, "api.remote_start"))
		ctl_toast(false, SET.remote_start ? "Remote START allowed - the panel counts down %d s first, Reset cancels" : "Remote START off - only the panel starts programs", SET.start_delay);
	if (!strcmp(d->key, "api.on")) ctl_toast(!SET.api, SET.api ? "REST API on - port %d" : "REST API off - nothing on the network can read or control this machine", SET.api_port);
}

void set_key(pkey_t k, bool sh)
{
	if (k == K_F1) { ctl_save(); ctl_toast(false, "Settings saved"); return; }
	if (k == K_F6) { inl.on = false; ui_goto(SCR_IO); return; }
	if (k == K_F5) { show_hint = !show_hint; return; }
	if (k == K_F2) {
		if (ctl_busy()) { ctl_toast(true, "Restore defaults when idle"); return; }
		settings_defaults(&SET); ctl_apply_axis_config(); settings_save(C.cfg_path);
		ctl_toast(false, "Restored this machine's original settings"); return;
	}
	if (k == K_F4) {
		int ok = settings_save("/udisk-sda1/pandora-settings.cfg") == 0 || settings_save("/udisk-sda/pandora-settings.cfg") == 0;
		ctl_toast(!ok, ok ? "Exported pandora-settings.cfg to USB" : "No USB stick found");
		return;
	}
	if (inl.on) {                                       /* inline edit in progress */
		if (k == K_YP || k == K_XP) inline_step(1);
		else if (k == K_YM || k == K_XM) inline_step(-1);
		else if (k == K_ENTER || k == K_SEL || k == K_MOD || k == K_F3) {
			inl.on = false;
			if (inl.d->machine && can(C_EDIT_SETTING, NULL, SRC_PANEL)) { ctl_toast(true, "Machine started moving - edit discarded"); return; }
			const char *clash = setting_conflict(inl.d, inl.v);
			if (clash) { ctl_toast(true, "%s", clash); return; }
			setting_set(inl.d, inl.v); changed(inl.d);
			char v[64]; setting_format(inl.d, v, sizeof v);
			ctl_toast(false, "%s = %s", inl.d->label, v);
		} else if (k == K_CANCEL) { inl.on = false; ctl_toast(false, "Edit discarded"); }
		return;
	}
	if (ui_pane() == 0) {
		if (k == K_YP || k == K_YM) { U.grp = CLAMP(U.grp + (k == K_YM ? 1 : -1), 0, ngroups() - 1); U.item = 0; }
		if (k == K_XP || k == K_ENTER || k == K_SEL || k == K_MOD) U.pane[SCR_SETTINGS] = 1;   /* quick press opens */
		return;
	}
	if (k == K_CANCEL || k == K_XM) { U.pane[SCR_SETTINGS] = 0; return; }        /* back to the categories */
	const setting_desc_t *it[64];
	int n = group_items(it, 64);
	bool start = k == K_ENTER || k == K_MOD || k == K_F3 || k == K_SEL;     /* incl. a pendant quick press */
	if (k == K_YP || k == K_YM) {
		/* one step past either end lands on "back to the groups" (-1 before the first, n after
		 * the last) and stays there; only Enter / a quick press there leaves the group, so
		 * scrolling to the bottom never leaves the category by surprise */
		if (U.item < 0) U.item = k == K_YM ? 0 : -1;
		else if (U.item >= n) U.item = k == K_YP ? n - 1 : n;
		else U.item += k == K_YM ? 1 : -1;
		if (n == 0) U.item = k == K_YM ? 0 : -1;
		return;
	}
	if (U.item < 0 || U.item >= n) {                    /* on the back position */
		if (start) { U.pane[SCR_SETTINGS] = 0; U.item = 0; }
		return;
	}
	const setting_desc_t *d = it[U.item];
	const char *na = inapplicable(d);
	if (start && na) { ctl_toast(true, "%s: not used - %s", d->label, na); return; }
	if (d->machine) { const char *w = can(C_EDIT_SETTING, NULL, SRC_PANEL); if (w) { ctl_toast(true, "%s", w); return; } }
	if (!start) return;
	if (is_inline(d)) { inl.on = true; inl.d = d; inl.v = setting_get(d); }
	else {
		int dec = setting_decimals(d), intd = 1;
		for (double m = d->max; m >= 10 && intd < 5; m /= 10) intd++;
		ui_editor_open(d->label, setting_get(d), dec, d->unit, intd < 2 ? 2 : intd, d->max, d->min < 0, d->machine, set_num, (int)(d - SETTINGS));
	}
}
