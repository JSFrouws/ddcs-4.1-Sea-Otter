/* Ethernet API (docs/ethernet-api.md). Every call that moves or changes the machine goes
 * through act(), the same as a panel key, so the state machine guards apply unchanged:
 * 409 when the state refuses it, 403 for START while remote start is disabled. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "api.h"
#include "../core/machine.h"
#include "../sim/fpga_sim.h"
#include "../ui/ui.h"
#include "../perf.h"

extern const char WEB_PANEL[];
extern const unsigned WEB_PANEL_LEN;
extern const char *UI_SRC_DIRS[3];

extern uint32_t bus_errors, bus_retries;
extern int bus_last_errno;
static display_t *DSP;
static panel_t *PNL;

void api_bind(display_t *d, panel_t *p) { DSP = d; PNL = p; }

/* ---- tiny JSON field readers for flat request bodies ---- */
static const char *jfind(const char *b, const char *key)
{
	char k[48];
	snprintf(k, sizeof k, "\"%s\"", key);
	const char *p = b ? strstr(b, k) : NULL;
	if (!p) return NULL;
	p = strchr(p + strlen(k), ':');
	if (!p) return NULL;
	p++;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
	return p;
}
static bool jnum(const char *b, const char *key, double *v) { const char *p = jfind(b, key); if (!p || (*p != '-' && *p != '.' && (*p < '0' || *p > '9'))) return false; *v = atof(p); return true; }
static bool jbool(const char *b, const char *key, bool *v) { const char *p = jfind(b, key); if (!p) return false; *v = !strncmp(p, "true", 4) || *p == '1'; return true; }
static bool jstr(const char *b, const char *key, char *out, size_t n)
{
	const char *p = jfind(b, key);
	if (!p || *p != '"') return false;
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i + 1 < n) { if (*p == '\\' && p[1]) p++; out[i++] = *p++; }
	out[i] = 0;
	return true;
}
static int axis_of(const char *s) { for (int a = 0; a < NAXES; a++) if (toupper((unsigned char)s[0]) == AXIS_CH[a]) return a; return -1; }

static void json_escape(char *o, size_t n, const char *s)
{
	size_t k = 0;
	for (; *s && k + 2 < n; s++) { if (*s == '"' || *s == '\\') o[k++] = '\\'; o[k++] = (unsigned char)*s < 32 ? ' ' : *s; }
	o[k] = 0;
}

static void refuse(http_resp_t *r, cmd_t c, const arg_t *a)
{
	const char *why = can(c, a, SRC_API);
	char e[200];
	json_escape(e, sizeof e, why ? why : "refused");
	resp_text(r, why && strstr(why, "remote START") ? 403 : 409, "application/json",
		"{\"error\":\"%s\",\"state\":\"%s\",\"detail\":\"%s\"}", why && strstr(why, "remote START") ? "remote_start_disabled" : "state", STATE_NAMES[C.m], e);
}

static void doit(http_resp_t *r, cmd_t c, const arg_t *a)
{
	if (can(c, a, SRC_API)) { refuse(r, c, a); return; }
	act(c, a, SRC_API);
	resp_text(r, 200, "application/json", "{\"ok\":true,\"state\":\"%s\"}", STATE_NAMES[C.m]);
}

static void status(http_resp_t *r)
{
	char pos[NAXES][48], wp[NAXES][48], nm[160], msg[200];
	for (int a = 0; a < NAXES; a++) { snprintf(pos[a], 48, "%.4f", C.mo.pos[a]); snprintf(wp[a], 48, "%.4f", ctl_work(a)); }
	json_escape(nm, sizeof nm, C.prog.name);
	json_escape(msg, sizeof msg, C.msg);
	char dw[200];
	json_escape(dw, sizeof dw, C.driver_warn);
	resp_text(r, 200, "application/json",
		"{\"state\":\"%s\",\"homed\":%s,\"machine\":{\"x\":%s,\"y\":%s,\"z\":%s,\"a\":%s},"
		"\"work\":{\"x\":%s,\"y\":%s,\"z\":%s,\"a\":%s},\"wcs\":\"G%d\",\"program\":{\"name\":\"%s\",\"loaded\":%s,\"lines\":%d,\"line\":%d},"
		"\"overrides\":{\"feed\":%d,\"rapid\":%d,\"spindle\":%d,\"feedApplied\":%.1f},\"spindle\":{\"on\":%s,\"rpm\":%.0f},\"coolant\":%d,"
		"\"limit\":{\"axis\":\"%c\",\"dir\":%d},\"limits\":{\"x\":%d,\"y\":%d,\"z\":%d,\"a\":%d},\"armed\":\"%s\",\"remoteStart\":%s,\"countdown\":%d,\"screen\":%d,\"leds\":%u,"
		"\"fpga\":{\"x\":%d,\"y\":%d,\"z\":%d,\"a\":%d},\"steps\":{\"x\":%lld,\"y\":%lld,\"z\":%lld,\"a\":%lld},"
		"\"posChecks\":%u,\"posFaults\":%u,\"posErr\":[%d,%d,%d,%d],\"faultLine\":%d,\"busErrors\":%u,\"busRetries\":%u,\"busErrno\":%d,\"reversalsWaited\":%u,\"bursts\":%u,"
		"\"perf\":{\"loops\":%u,\"tickUs\":%u,\"tickUsMax\":%u,\"renderUs\":%u,\"renderUsMax\":%u,\"drawUs\":%u,\"presentUs\":%u,\"gapMsMax\":%u,\"motionMs\":%llu,\"lostMs\":%llu,\"replanIters\":%u},\"message\":\"%s\",\"driverWarning\":\"%s\",\"backend\":\"%s\",\"frame\":%u}",
		STATE_NAMES[C.m], C.homed ? "true" : "false", pos[0], pos[1], pos[2], pos[3], wp[0], wp[1], wp[2], wp[3], 54 + WD.active,
		nm, C.prog.loaded ? "true" : "false", C.prog.nlines, ctl_cur_line() + 1,
		C.mo.feed_ov, rapid_override(C.mo.feed_ov), C.mo.spin_ov, C.mo.applied_pct, C.spindle ? "true" : "false", C.rpm, C.coolant,
		C.limit_axis >= 0 ? AXIS_CH[C.limit_axis] : '-', C.limit_dir, C.lim_dir[0], C.lim_dir[1], C.lim_dir[2], C.lim_dir[3], (const char *[]){ "run", "tryCut", "simulate" }[C.armed],
		SET.remote_start ? "true" : "false", C.countdown, U.screen, (unsigned)ui_leds(),
		C.fpos[0], C.fpos[1], C.fpos[2], C.fpos[3], (long long)C.mo.steps[0], (long long)C.mo.steps[1], (long long)C.mo.steps[2], (long long)C.mo.steps[3],
		C.pos_checks, C.pos_faults, C.pos_err[0], C.pos_err[1], C.pos_err[2], C.pos_err[3], C.fault_line, bus_errors, bus_retries, bus_last_errno, C.mo.deferred, C.fp.bursts, PERF.loops, PERF.tick_us_avg, PERF.tick_us_max, PERF.render_us_avg, PERF.render_us_max, PERF.draw_us_avg, PERF.present_us_avg, PERF.gap_ms_max,
		(unsigned long long)PERF.motion_ms, (unsigned long long)PERF.lost_ms, C.mo.replan_iters, msg, dw, C.sim ? "sim" : "real", DSP ? DSP->frame_no : 0);
}

static void settings_json(http_resp_t *r)
{
	size_t cap = 16384, n = 0;
	char *b = malloc(cap);
	n += (size_t)snprintf(b + n, cap - n, "{");
	for (int i = 0; i < NSETTINGS; i++)
		n += (size_t)snprintf(b + n, cap - n, "%s\"%s\":%.6g", i ? "," : "", SETTINGS[i].key, setting_get(&SETTINGS[i]));
	n += (size_t)snprintf(b + n, cap - n, "}");
	r->body = b; r->len = n; r->status = 200; r->type = "application/json";
}

static void put_settings(const char *body, http_resp_t *r)
{
	if (ctl_busy()) { refuse(r, C_EDIT_SETTING, NULL); return; }
	int changed = 0;
	bool axis = false;
	for (int i = 0; i < NSETTINGS; i++) {
		double v; bool bv;
		const setting_desc_t *d = &SETTINGS[i];
		bool has = jnum(body, d->key, &v);
		if (!has && jbool(body, d->key, &bv)) { v = bv; has = true; }
		if (!has) continue;
		if (d->api_ro && strcmp(d->key, "api.agent_msgs")) { resp_text(r, 403, "application/json", "{\"error\":\"read_only\",\"detail\":\"%s can only be changed on the panel\"}", d->key); return; }
		if (!strncmp(d->key, "axis.", 5)) {
			/* axis settings re-initialise the FPGA: never in the middle of a jog */
			if (!motion_idle(&C.mo)) { resp_text(r, 409, "application/json", "{\"error\":\"state\",\"detail\":\"wait for the jog to finish before changing %s\"}", d->key); return; }
			axis = true;
		}
		setting_set(d, v); changed++;
	}
	settings_save(C.cfg_path);
	if (axis) ctl_apply_axis_config();
	resp_text(r, 200, "application/json", "{\"ok\":true,\"changed\":%d}", changed);
}

static void instructions(const char *b, http_resp_t *r)
{
	agent_t *g = &C.agent;
	memset(g, 0, sizeof *g);
	jstr(b, "title", g->title, sizeof g->title);
	if (!jstr(b, "who", g->who, sizeof g->who)) snprintf(g->who, sizeof g->who, "agent");
	const char *p = b;
	while (g->n < AGENT_STEPS && (p = strstr(p, "\"text\""))) {
		jstr(p, "text", g->step[g->n].text, sizeof g->step[0].text);
		const char *nx = strstr(p + 6, "\"text\"");
		const char *ck = strstr(p, "\"check\"");
		if (ck && (!nx || ck < nx)) jstr(ck, "check", g->step[g->n].check, sizeof g->step[0].check);
		g->n++; p += 6;
	}
	g->on = true;
	resp_text(r, 200, "application/json", "{\"ok\":true,\"steps\":%d}", g->n);
}

static void upload(const http_req_t *q, http_resp_t *r)
{
	char name[128] = "upload.nc";
	const char *n = strstr(q->query, "name=");
	if (n) { size_t i = 0; n += 5; while (*n && *n != '&' && i + 1 < sizeof name) { name[i++] = (*n == '/' || *n == '\\') ? '_' : *n; n++; } name[i] = 0; }
	char path[320];
	snprintf(path, sizeof path, "%s/%s", UI_SRC_DIRS[0], name);
	FILE *f = fopen(path, "wb");
	if (f) { fwrite(q->body, 1, q->body_len, f); fclose(f); }
	arg_t a = { .path = name, .text = q->body, .len = q->body_len };
	if (can(C_LOAD, &a, SRC_API)) { refuse(r, C_LOAD, &a); return; }
	act(C_LOAD, &a, SRC_API);
	if (!C.prog.loaded) { char e[160]; json_escape(e, sizeof e, C.prog.err); resp_text(r, 400, "application/json", "{\"error\":\"gcode\",\"detail\":\"%s\",\"line\":%d}", e, C.prog.err_line + 1); return; }
	ui_scan_files();
	resp_text(r, 200, "application/json", "{\"ok\":true,\"name\":\"%s\",\"lines\":%d,\"saved\":%s}", name, C.prog.nlines, f ? "true" : "false");
}

static void run_start(const char *b, http_resp_t *r)
{
	char mode[16] = "";
	if (jstr(b, "mode", mode, sizeof mode) && C.m == M_IDLE)
		C.armed = !strcasecmp(mode, "tryCut") ? ARM_TRYCUT : !strcasecmp(mode, "simulate") ? ARM_SIM : ARM_RUN;
	if (can(C_START, NULL, SRC_API)) { refuse(r, C_START, NULL); return; }
	if (SET.start_delay > 0 && C.m == M_IDLE) {
		C.countdown = SET.start_delay; C.countdown_t = now_ms();
		resp_text(r, 202, "application/json", "{\"ok\":true,\"countdown\":%d,\"detail\":\"panel counts down, Reset cancels\"}", C.countdown);
		return;
	}
	doit(r, C_START, NULL);
}

void api_handle(const http_req_t *q, http_resp_t *r)
{
	const char *p = q->path, *b = q->body;
	bool post = !strcmp(q->method, "POST") || !strcmp(q->method, "PUT"), del = !strcmp(q->method, "DELETE");
	if (!strcmp(p, "/") || !strcmp(p, "/panel")) { r->status = 200; r->type = "text/html; charset=utf-8"; r->static_body = WEB_PANEL; r->len = WEB_PANEL_LEN; return; }
	if (!strcmp(p, "/frame")) {
		r->status = 200; r->type = "application/octet-stream";
		r->static_body = DSP->front; r->len = SCR_W * SCR_H * 2;
		static char h[64];
		snprintf(h, sizeof h, "X-Frame: %u\r\nX-Leds: %u\r\n", DSP->frame_no, (unsigned)ui_leds());
		r->extra_headers = h;
		return;
	}
	if (strncmp(p, "/api/v1/", 8)) { resp_text(r, 404, "text/plain", "not found"); return; }
	p += 8;
	if (!SET.api) { resp_text(r, 403, "application/json", "{\"error\":\"api_off\"}"); return; }
	if (!strcmp(p, "status")) { status(r); return; }
	if (!strcmp(p, "settings") && !post) { settings_json(r); return; }
	if (!strcmp(p, "settings")) { put_settings(b, r); return; }
	if (!post && !del) { resp_text(r, 404, "text/plain", "not found"); return; }

	arg_t a = { 0 };
	char s[64];
	double v;
	if (!strcmp(p, "key")) {                             /* remote / simulated panel key */
		pkey_t k = jstr(b, "key", s, sizeof s) ? key_by_name(s) : K_NONE;
		bool down = true;
		jbool(b, "down", &down);
		if (k == K_NONE) { resp_text(r, 400, "application/json", "{\"error\":\"unknown key\"}"); return; }
		/* no shortcut around the remote-start rule: a remote Start key press on the Run
		 * screen of a real machine counts as an API start */
		if (!C.sim && down && k == K_F1 && U.screen == SCR_RUN && !SET.remote_start) { refuse(r, C_START, NULL); return; }
		int bit = panel_key_to_bit(k);
		if (PNL && bit >= 0) { if (down) PNL->sim_keys |= 1u << bit; else PNL->sim_keys &= ~(1u << bit); }
		resp_text(r, 200, "application/json", "{\"ok\":true}");
		return;
	}
	if (!strcmp(p, "sim")) {                              /* simulator controls (sim backend only) */
		if (!C.sim) { resp_text(r, 409, "application/json", "{\"error\":\"not_simulated\"}"); return; }
		bool bv;
		if (jbool(b, "held", &bv)) SIM.pendant_held = bv;
		if (jnum(b, "axis", &v)) SIM.pendant_axis = (int)v;
		if (jnum(b, "mult", &v)) SIM.pendant_mult = (int)v;
		if (jnum(b, "wheel", &v)) SIM.wheel = (uint16_t)(SIM.wheel + (int)v);
		if (jbool(b, "pendantEstop", &bv)) SIM.pendant_estop = bv;
		if (jbool(b, "estop", &bv)) SIM.ext_estop = bv;
		if (jstr(b, "limit", s, sizeof s)) { int ax = axis_of(s); if (ax >= 0) SIM.switch_at[ax] = SIM.pos[ax] + (SIM.switch_side[ax] < 0 ? 4000 : -4000); }
		resp_text(r, 200, "application/json", "{\"ok\":true}");
		return;
	}
	if (!strcmp(p, "machine/stop")) {
		int32_t at[NAXES];
		fpga_heartbeat(&C.fp); fpga_positions(&C.fp, at);        /* where the FPGA is right now */
		if (ctl_busy() || C.m == M_LIMIT || !motion_idle(&C.mo)) act(C_STOP, NULL, SRC_API);
		C.countdown = 0;
		resp_text(r, 200, "application/json", "{\"ok\":true,\"state\":\"%s\",\"fpgaAtStop\":{\"x\":%d,\"y\":%d,\"z\":%d,\"a\":%d}}",
			STATE_NAMES[C.m], at[0], at[1], at[2], at[3]);
		return;
	}
	if (!strcmp(p, "machine/drives")) { bool on = true; jbool(b, "on", &on); doit(r, on ? C_DRIVES_ON : C_DRIVES_OFF, NULL); return; }
	if (!strcmp(p, "machine/home")) { doit(r, C_HOME, NULL); return; }
	if (!strcmp(p, "jog")) {
		if (!jstr(b, "axis", s, sizeof s) || (a.axis = axis_of(s)) < 0 || !jnum(b, "dist", &v)) { resp_text(r, 400, "application/json", "{\"error\":\"need axis and dist\"}"); return; }
		a.dir = v < 0 ? -1 : 1; a.dist = fabs(v);
		doit(r, C_JOG, &a); return;
	}
	if (!strcmp(p, "move/gotozero")) { doit(r, C_GOTOZERO, NULL); return; }
	if (!strcmp(p, "move/macro")) {                    /* {"macro":"park"} or {"macro":2} */
		static const char *const ids[MAC__N] = { "safez", "workzero", "park", "toolchange", "warmup" };
		a.kind = -1;
		if (jstr(b, "macro", s, sizeof s)) for (int i = 0; i < MAC__N; i++) if (!strcmp(s, ids[i])) a.kind = i;
		if (a.kind < 0 && jnum(b, "macro", &v)) a.kind = (int)v;
		doit(r, C_MACRO, &a); return;
	}
	if (!strcmp(p, "offsets/touch-off")) {
		if (!jstr(b, "axis", s, sizeof s) || (a.axis = axis_of(s)) < 0) { resp_text(r, 400, "application/json", "{\"error\":\"need axis\"}"); return; }
		a.dir = jnum(b, "dir", &v) && v < 0 ? -1 : 1;
		if (jnum(b, "diameter", &v)) SET.edge_dia = v;
		doit(r, C_TOUCHOFF, &a); return;
	}
	if (!strcmp(p, "offsets/active")) {
		if (can(C_EDIT_TABLE, NULL, SRC_API)) { refuse(r, C_EDIT_TABLE, NULL); return; }
		if (jstr(b, "wcs", s, sizeof s) && atoi(s + 1) >= 54 && atoi(s + 1) < 54 + NWCS) WD.active = atoi(s + 1) - 54;
		workdata_save(C.wd_path);
		resp_text(r, 200, "application/json", "{\"ok\":true,\"wcs\":\"G%d\"}", 54 + WD.active); return;
	}
	if (!strncmp(p, "offsets/G", 9)) {
		int i = atoi(p + 9) - 54;
		if (i < 0 || i >= NWCS) { resp_text(r, 404, "application/json", "{\"error\":\"no such WCS\"}"); return; }
		if (can(C_EDIT_TABLE, NULL, SRC_API)) { refuse(r, C_EDIT_TABLE, NULL); return; }
		const char *k[NAXES] = { "x", "y", "z", "a" };
		for (int ax = 0; ax < NAXES; ax++) if (jnum(b, k[ax], &v)) WD.wcs[i].off[ax] = v;
		bool bv; if (jbool(b, "job", &bv)) WD.wcs[i].job = bv;
		workdata_save(C.wd_path);
		resp_text(r, 200, "application/json", "{\"ok\":true}"); return;
	}
	if (!strcmp(p, "program/upload")) { upload(q, r); return; }
	if (!strcmp(p, "program/load")) {
		char path[320];
		if (!jstr(b, "name", s, sizeof s) || strchr(s, '/')) { resp_text(r, 400, "application/json", "{\"error\":\"need a file name\"}"); return; }
		snprintf(path, sizeof path, "%s/%s", UI_SRC_DIRS[0], s);
		a.path = path;
		if (can(C_LOAD, &a, SRC_API)) { refuse(r, C_LOAD, &a); return; }
		act(C_LOAD, &a, SRC_API);
		resp_text(r, C.prog.loaded ? 200 : 400, "application/json", "{\"ok\":%s,\"lines\":%d}", C.prog.loaded ? "true" : "false", C.prog.nlines); return;
	}
	if (!strcmp(p, "run/start")) { run_start(b, r); return; }
	if (!strcmp(p, "run/pause")) { doit(r, C_PAUSE, NULL); return; }
	if (!strcmp(p, "run/stop")) { doit(r, C_STOP, NULL); return; }
	if (!strcmp(p, "run/resume-from")) {
		if (!jnum(b, "line", &v) || v < 1) { resp_text(r, 400, "application/json", "{\"error\":\"need line (1-based)\"}"); return; }
		if (C.m != M_IDLE) { refuse(r, C_START, NULL); return; }
		C.resume_line = (int)v - 1;
		run_start(b, r); return;
	}
	if (!strcmp(p, "overrides")) {
		if (jnum(b, "feed", &v)) { a.kind = 0; a.delta = (int)v - C.mo.feed_ov; if (can(C_OVERRIDE, &a, SRC_API)) { refuse(r, C_OVERRIDE, &a); return; } act(C_OVERRIDE, &a, SRC_API); }
		if (jnum(b, "spindle", &v)) { a.kind = 1; a.delta = (int)v - C.mo.spin_ov; act(C_OVERRIDE, &a, SRC_API); }
		resp_text(r, 200, "application/json", "{\"feed\":%d,\"rapid\":%d,\"spindle\":%d}", C.mo.feed_ov, rapid_override(C.mo.feed_ov), C.mo.spin_ov); return;
	}
	if (!strcmp(p, "panel/instructions")) {
		if (del) { C.agent.on = false; resp_text(r, 200, "application/json", "{\"ok\":true}"); return; }
		instructions(b, r); return;
	}
	if (!strcmp(p, "panel/toast")) { if (jstr(b, "text", s, sizeof s)) ctl_toast(false, "%s", s); resp_text(r, 200, "application/json", "{\"ok\":true}"); return; }
	resp_text(r, 404, "application/json", "{\"error\":\"unknown endpoint\"}");
}
