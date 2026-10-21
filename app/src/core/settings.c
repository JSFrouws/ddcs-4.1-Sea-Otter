#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"

settings_t SET;
workdata_t WD;

const char *const SETTING_GROUPS[] = {
	"Display", "Jog & touch-off", "Pendant", "Network & API", "Axes", "Homing & limits",
	"Spindle & outputs", "Macros", "I/O & diagnostics", NULL
};
const char *const HOME_ORDERS[] = { "Z X Y A", "Z X Y", "Z Y X A", "X Y Z A", NULL };
static const char *const DECS[] = { "0.0", "0.00", "0.000", NULL };
static const char *const VIEWS[] = { "2D plan", "Isometric wireframe", "CPU saver (no preview)", NULL };
static const char *const DIRS[] = { "Negative", "Positive", NULL };

#define O(f) offsetof(settings_t, f)
#define A(i, f) (offsetof(settings_t, ax) + (i) * sizeof(axis_set_t) + offsetof(axis_set_t, f))
#define AXROWS(i, n) \
	{ "axis." n ".steps", "Axes", n " steps/mm", "", ST_DBL, A(i, steps_per_mm), 1, 100000, 1, 0, 1, 0 }, \
	{ "axis." n ".max_vel", "Axes", n " max speed", "mm/min", ST_DBL, A(i, max_vel), 1, 30000, 0, 0, 1, 0 }, \
	{ "axis." n ".accel", "Axes", n " acceleration", "mm/s²", ST_DBL, A(i, accel), 1, 10000, 0, 0, 1, 0 }, \
	{ "axis." n ".jerk", "Axes", n " jerk (S-curve)", "mm/s³", ST_DBL, A(i, jerk), 10, 1000000, 0, 0, 1, 0 }, \
	{ "axis." n ".corner_jump", "Axes", n " corner speed jump", "mm/min", ST_DBL, A(i, corner_jump), 0, 6000, 0, 0, 1, 0 }, \
	{ "axis." n ".dir_invert", "Axes", n " DIR invert", "", ST_BOOL, A(i, dir_invert), 0, 1, 0, 0, 1, 0 }, \
	{ "axis." n ".soft_min", "Homing & limits", n " soft limit −", "mm", ST_DBL, A(i, soft_min), -9999, 9999, DEC_POS, 0, 1, 0 }, \
	{ "axis." n ".soft_max", "Homing & limits", n " soft limit +", "mm", ST_DBL, A(i, soft_max), -9999, 9999, DEC_POS, 0, 1, 0 }, \
	{ "axis." n ".bench_min", "Homing & limits", n " bench limit −", "mm", ST_DBL, A(i, bench_min), -99999, 99999, DEC_POS, 0, 1, 0 }, \
	{ "axis." n ".bench_max", "Homing & limits", n " bench limit +", "mm", ST_DBL, A(i, bench_max), -99999, 99999, DEC_POS, 0, 1, 0 }, \
	{ "axis." n ".limit_port", "Homing & limits", n " limit/home switch input (IN, 0 = none)", "", ST_INT, A(i, home_port), 0, 18, 0, 0, 1, 0 }, \
	{ "axis." n ".home_dir", "Homing & limits", n " home direction", "", ST_ENUM, A(i, home_dir), 0, 1, 0, DIRS, 1, 0 }, \
	{ "axis." n ".home_pos", "Homing & limits", n " position at switch", "mm", ST_DBL, A(i, home_pos), -9999, 9999, DEC_POS, 0, 1, 0 }, \
	{ "axis." n ".home_backoff", "Homing & limits", n " back-off after home", "mm", ST_DBL, A(i, home_backoff), 0, 500, DEC_POS, 0, 1, 0 }

const setting_desc_t SETTINGS[] = {
	{ "display.pos_dec", "Display", "Position decimals (X Y Z A)", "", ST_ENUM, O(pos_dec), 1, 3, 0, DECS, 0, 0 },
	{ "display.tool_dec", "Display", "Tool size decimals (Ø, length)", "", ST_ENUM, O(tool_dec), 1, 3, 0, DECS, 0, 0 },
	{ "display.view", "Display", "Default toolpath view", "", ST_ENUM, O(view_iso), 0, 2, 0, VIEWS, 0, 0 },
	{ "jog.edge_dia", "Jog & touch-off", "Edge finder / tool Ø", "mm", ST_DBL, O(edge_dia), 0, 50, DEC_TOOL, 0, 0, 0 },
	{ "jog.feed", "Jog & touch-off", "Jog feed", "mm/min", ST_DBL, O(jog_feed), 1, 6000, 0, 0, 1, 0 },
	{ "jog.safe_z", "Jog & touch-off", "Safe Z above work zero", "mm", ST_DBL, O(safe_z), 0, 200, DEC_POS, 0, 1, 0 },
	{ "motion.graceful_stop", "Homing & limits", "Controlled stop on Reset, E-stop, limit (no lost steps)", "", ST_BOOL, O(graceful_stop), 0, 1, 0, 0, 1, 0 },
	{ "planner.lookahead", "Axes", "Planner lookahead (segments)", "", ST_INT, O(lookahead), 4, 400, 0, 0, 1, 0 },
	{ "planner.junction_dev", "Axes", "Corner tolerance", "mm", ST_DBL, O(junction_dev), 0, 1, 3, 0, 1, 0 },
	{ "gcode.default_feed", "Jog & touch-off", "Feed when a program sets none", "mm/min", ST_DBL, O(default_feed), 1, 20000, 0, 0, 1, 0 },
	{ "pendant.on", "Pendant", "MPG pendant fitted (optional)", "", ST_BOOL, O(pendant), 0, 1, 0, 0, 0, 0 },
	{ "pendant.quick_ms", "Pendant", "Quick press = Enter, shorter than", "ms", ST_INT, O(quick_ms), 100, 1500, 0, 0, 0, 0 },
	{ "pendant.wheel_div", "Pendant", "Counts per wheel detent", "", ST_INT, O(wheel_div), 1, 8, 0, 0, 0, 0 },
	{ "pendant.wheel_reverse", "Pendant", "Reverse wheel direction", "", ST_BOOL, O(wheel_reverse), 0, 1, 0, 0, 0, 0 },
	{ "pendant.estop", "Pendant", "Pendant E-stop enabled", "", ST_BOOL, O(pendant_estop), 0, 1, 0, 0, 1, 0 },
	{ "api.on", "Network & API", "API (HTTP, port below)", "", ST_BOOL, O(api), 0, 1, 0, 0, 0, 1 },
	{ "api.port", "Network & API", "API port", "", ST_INT, O(api_port), 1, 65535, 0, 0, 0, 1 },
	{ "api.remote_start", "Network & API", "Allow remote START (agents, scripts)", "", ST_BOOL, O(remote_start), 0, 1, 0, 0, 0, 1 },
	{ "api.start_delay", "Network & API", "Remote start countdown", "s", ST_INT, O(start_delay), 0, 30, 0, 0, 0, 1 },
	{ "api.agent_msgs", "Network & API", "Show agent instructions on panel", "", ST_BOOL, O(agent_msgs), 0, 1, 0, 0, 0, 0 },
	AXROWS(0, "X"), AXROWS(1, "Y"), AXROWS(2, "Z"), AXROWS(3, "A"),
	{ "home.order", "Homing & limits", "Home order", "", ST_ENUM, O(home_order), 0, 3, 0, HOME_ORDERS, 1, 0 },
	{ "limits.soft", "Homing & limits", "Soft limits (when homed)", "", ST_BOOL, O(soft_limits), 0, 1, 0, 0, 1, 0 },
	{ "limits.bench", "Homing & limits", "Bench limits (work box, also un-homed)", "", ST_BOOL, O(bench_limits), 0, 1, 0, 0, 1, 0 },
	{ "limits.level_high", "Homing & limits", "Limit/home active when open (NC)", "", ST_BOOL, O(limit_level_high), 0, 1, 0, 0, 1, 0 },
	{ "estop.port", "Homing & limits", "External E-stop input (0 = none)", "IN", ST_INT, O(estop_port), 0, 18, 0, 0, 1, 0 },
	{ "estop.level_high", "Homing & limits", "E-stop active when open (NC)", "", ST_BOOL, O(estop_level_high), 0, 1, 0, 0, 1, 0 },
	{ "spindle.max", "Spindle & outputs", "Spindle max RPM (10 V)", "rpm", ST_DBL, O(spindle_max), 100, 60000, 0, 0, 1, 0 },
	{ "spindle.default", "Spindle & outputs", "Spindle RPM when none given", "rpm", ST_DBL, O(spindle_default), 0, 60000, 0, 0, 1, 0 },
	{ "spindle.delay", "Spindle & outputs", "Spin-up wait", "s", ST_DBL, O(spindle_delay), 0, 30, 1, 0, 1, 0 },
	{ "out.spindle", "Spindle & outputs", "M3 output (OUT n, 0 = none)", "", ST_INT, O(out_spindle), 0, 3, 0, 0, 1, 0 },
	{ "out.flood", "Spindle & outputs", "M8 output", "", ST_INT, O(out_flood), 0, 3, 0, 0, 1, 0 },
	{ "out.aux", "Spindle & outputs", "M10 output", "", ST_INT, O(out_aux), 0, 3, 0, 0, 1, 0 },
	{ "macro.park_x", "Macros", "Park X (machine)", "mm", ST_DBL, O(park[0]), -9999, 9999, DEC_POS, 0, 1, 0 },
	{ "macro.park_y", "Macros", "Park Y (machine)", "mm", ST_DBL, O(park[1]), -9999, 9999, DEC_POS, 0, 1, 0 },
	{ "macro.park_z", "Macros", "Park Z (machine)", "mm", ST_DBL, O(park[2]), -9999, 9999, DEC_POS, 0, 1, 0 },
	{ "macro.tc_x", "Macros", "Tool change X (machine)", "mm", ST_DBL, O(tc[0]), -9999, 9999, DEC_POS, 0, 1, 0 },
	{ "macro.tc_y", "Macros", "Tool change Y (machine)", "mm", ST_DBL, O(tc[1]), -9999, 9999, DEC_POS, 0, 1, 0 },
	{ "macro.tc_z", "Macros", "Tool change Z (machine)", "mm", ST_DBL, O(tc[2]), -9999, 9999, DEC_POS, 0, 1, 0 },
	{ "macro.warm_rpm", "Macros", "Warm-up spindle speed", "rpm", ST_DBL, O(warm_rpm), 0, 60000, 0, 0, 1, 0 },
	{ "macro.warm_s", "Macros", "Warm-up time", "s", ST_DBL, O(warm_s), 1, 3600, 0, 0, 1, 0 },
};
const int NSETTINGS = ARRAY_LEN(SETTINGS);

void settings_defaults(settings_t *s)
{
	memset(s, 0, sizeof *s);
	/* docs/machine-settings.md: steps #2-5, speeds #91-94, soft limits #235-242,
	 * home #199-221 (home_pos is ours: vendor #214/215 semantics are unverified) */
	const double smin[NAXES] = { 10, 10, -300, -99999 }, smax[NAXES] = { 915, 590, -10, 99999 };
	const double back[NAXES] = { 50, 50, 15, 0 };
	const int hdir[NAXES] = { 0, 0, 1, 0 }, lim[NAXES] = { 2, 3, 4, 0 }, alarm[NAXES] = { 10, 11, 12, 13 };
	for (int i = 0; i < NAXES; i++) {
		axis_set_t *a = &s->ax[i];
		a->steps_per_mm = 2000; a->max_vel = 1200; a->accel = 300;   /* 1200 = vendor #91-94 = FPGA cap */
		a->jerk = 10000; a->corner_jump = 300;   /* jerk: 30 ms acceleration ramp; 3000 cost ~20 % on the rosette */
		a->soft_min = smin[i]; a->soft_max = smax[i];
		a->bench_min = smin[i]; a->bench_max = smax[i];       /* starts as the soft box: narrow it to the bench */
		a->home_search = i == AX_A ? 700 : 1000; a->home_locate = 50; a->home_pos = 0;
		a->home_backoff = back[i]; a->home_dir = hdir[i];
		a->dir_invert = i == AX_X; a->pulse_high = true; a->enabled = true;
		a->limit_port = lim[i]; a->home_port = lim[i]; a->alarm_port = alarm[i];
	}
	s->home_order = 0; s->soft_limits = true;
	s->limit_level_high = true; s->estop_port = 1; s->estop_level_high = false;
	s->pendant_estop = true;
	s->spindle_max = 24000; s->spindle_default = 12000; s->spindle_delay = 2;
	s->out_spindle = 1; s->out_flood = 2; s->out_aux = 3;
	s->safe_z = 5; s->jog_feed = 1200; s->default_feed = 1000;
	s->lookahead = 64; s->junction_dev = 0.02;
	s->park[0] = 900; s->park[1] = 580; s->park[2] = -15;     /* back corner, Z up (inside the soft limits) */
	s->tc[0] = 450; s->tc[1] = 20; s->tc[2] = -15;           /* front middle, Z up */
	s->warm_rpm = 8000; s->warm_s = 120;
	s->graceful_stop = true;   /* user 2026-10-18: brake at max deceleration, never lose steps */
	s->pos_dec = 3; s->tool_dec = 2; s->view_iso = 0; s->edge_dia = 0;
	s->pendant = true; s->quick_ms = 400; s->wheel_div = 4;
	s->wheel_reverse = true;   /* user, 2026-10-18: clockwise (numbers up) must move + and increase values */
	s->api = true; s->api_port = 8080; s->remote_start = true; s->start_delay = 5;   /* user 2026-10-18: on by default, can be switched off */ s->agent_msgs = true;
}

double setting_get(const setting_desc_t *d)
{
	const char *p = (const char *)&SET + d->off;
	switch (d->type) {
	case ST_DBL: return *(const double *)p;
	case ST_BOOL: return *(const bool *)p;
	default: return *(const int *)p;
	}
}

void setting_set(const setting_desc_t *d, double v)
{
	char *p = (char *)&SET + d->off;
	v = CLAMP(v, d->min, d->max);
	switch (d->type) {
	case ST_DBL: *(double *)p = v; break;
	case ST_BOOL: *(bool *)p = v != 0; break;
	default: *(int *)p = (int)(v + (v < 0 ? -0.5 : 0.5)); break;
	}
}

const setting_desc_t *setting_find(const char *key)
{
	for (int i = 0; i < NSETTINGS; i++)
		if (!strcmp(SETTINGS[i].key, key)) return &SETTINGS[i];
	return NULL;
}

int setting_decimals(const setting_desc_t *d)
{
	return d->dec == DEC_POS ? SET.pos_dec : d->dec == DEC_TOOL ? SET.tool_dec : d->dec;
}

void setting_format(const setting_desc_t *d, char *b, size_t n)
{
	double v = setting_get(d);
	if (d->type == ST_BOOL) snprintf(b, n, "%s", v ? "ON" : "OFF");
	else if (d->type == ST_ENUM) snprintf(b, n, "%s", d->opts[(int)v - (int)d->min]);
	else snprintf(b, n, "%.*f%s%s", setting_decimals(d), v, *d->unit ? " " : "", d->unit);
}

int settings_save(const char *path)
{
	FILE *f = fopen(path, "w");
	if (!f) return -1;
	fprintf(f, "# pandora settings - key=value, unknown keys are ignored\n");
	for (int i = 0; i < NSETTINGS; i++)
		fprintf(f, "%s=%.6g\n", SETTINGS[i].key, setting_get(&SETTINGS[i]));
	fclose(f);
	return 0;
}

int settings_load(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq) continue;
		*eq = 0;
		const setting_desc_t *d = setting_find(line);
		if (d) setting_set(d, atof(eq + 1));
	}
	fclose(f);
	return 0;
}

/* ---- work data ---- */
void workdata_defaults(workdata_t *w)
{
	memset(w, 0, sizeof *w);
	const double g54[NAXES] = { 273.510, 137.703, -96.201, 0 };
	memcpy(w->wcs[0].off, g54, sizeof g54);
	w->wcs[0].job = true;
	static const struct { double d, l; const char *n; } t[] = {
		{ 3.175, 52.41, "1/8\" downcut" }, { 4, 48.9, "4 mm ball" }, { 6, 61.205, "6 mm flat 2F" },
		{ 6, 58.33, "6 mm flat 3F" }, { 0, 44.1, "90° V-bit" }, { 12, 0, "edge finder" } };
	for (size_t i = 0; i < ARRAY_LEN(t); i++) {
		w->tool[i].dia = t[i].d; w->tool[i].len = t[i].l;
		snprintf(w->tool[i].note, sizeof w->tool[i].note, "%s", t[i].n);
	}
	w->loaded_tool = 3;
}

int workdata_save(const char *path)
{
	FILE *f = fopen(path, "w");
	if (!f) return -1;
	fprintf(f, "active=%d\nloaded_tool=%d\n", WD.active, WD.loaded_tool);
	for (int i = 0; i < NWCS; i++)
		fprintf(f, "wcs%d=%.4f %.4f %.4f %.4f %d\n", 54 + i, WD.wcs[i].off[0], WD.wcs[i].off[1],
			WD.wcs[i].off[2], WD.wcs[i].off[3], WD.wcs[i].job);
	for (int i = 0; i < NTOOLS; i++)
		fprintf(f, "tool%d=%.4f %.4f %s\n", i + 1, WD.tool[i].dia, WD.tool[i].len, WD.tool[i].note);
	fclose(f);
	return 0;
}

int workdata_load(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	char line[160];
	while (fgets(line, sizeof line, f)) {
		int n, job;
		double a, b, c, d;
		char note[64] = "";
		line[strcspn(line, "\r\n")] = 0;
		if (sscanf(line, "active=%d", &n) == 1) WD.active = CLAMP(n, 0, NWCS - 1);
		else if (sscanf(line, "loaded_tool=%d", &n) == 1) WD.loaded_tool = CLAMP(n, 0, NTOOLS);
		else if (sscanf(line, "wcs%d=%lf %lf %lf %lf %d", &n, &a, &b, &c, &d, &job) == 6 && n >= 54 && n < 54 + NWCS) {
			wcs_t *w = &WD.wcs[n - 54];
			w->off[0] = a; w->off[1] = b; w->off[2] = c; w->off[3] = d; w->job = job;
		} else if (sscanf(line, "tool%d=%lf %lf %63[^\n]", &n, &a, &b, note) >= 3 && n >= 1 && n <= NTOOLS) {
			WD.tool[n - 1].dia = a; WD.tool[n - 1].len = b;
			snprintf(WD.tool[n - 1].note, sizeof WD.tool[n - 1].note, "%.23s", note);
		}
	}
	fclose(f);
	return 0;
}
