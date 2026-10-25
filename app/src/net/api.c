/* Ethernet API (docs/ethernet-api.md). Every call that moves or changes the machine goes
 * through act(), the same as a panel key, so the state machine guards apply unchanged:
 * 409 when the state refuses it, 403 for START while remote start is disabled. */
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "api.h"
#include "../core/machine.h"
#include "../core/macro.h"
#include "../core/estimate.h"
#include "../sim/fpga_sim.h"
#include "../ui/ui.h"
#include "../perf.h"

extern const char WEB_PANEL[];
extern const unsigned WEB_PANEL_LEN;
extern const char *UI_SRC_DIRS[3];
extern const char *const UI_SRC_NAMES[3];

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

/* one of the API permissions (Settings > API, panel only): false = answered 403 */
static bool allowed(http_resp_t *r, bool on, const char *what, const char *key)
{
	if (on) return true;
	resp_text(r, 403, "application/json", "{\"error\":\"not_allowed\",\"detail\":\"%s over the API is switched off on the panel (Settings > API > %s)\",\"setting\":\"%s\"}", what, key, key);
	return false;
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
	char dw[200], mac[100];
	json_escape(dw, sizeof dw, C.driver_warn);
	json_escape(mac, sizeof mac, C.macro_on ? C.macro_name : "");
	bool est_left;
	double est_t_now = est_now(&est_left);
	resp_text(r, 200, "application/json",
		"{\"state\":\"%s\",\"homed\":%s,\"machine\":{\"x\":%s,\"y\":%s,\"z\":%s,\"a\":%s},"
		"\"work\":{\"x\":%s,\"y\":%s,\"z\":%s,\"a\":%s},\"wcs\":\"G%d\",\"program\":{\"name\":\"%s\",\"loaded\":%s,\"lines\":%d,\"line\":%d},"
		"\"overrides\":{\"feed\":%d,\"rapid\":%d,\"spindle\":%d,\"feedApplied\":%.1f},\"spindle\":{\"on\":%s,\"rpm\":%.0f},\"coolant\":%d,"
		"\"limit\":{\"axis\":\"%c\",\"dir\":%d},\"limits\":{\"x\":%d,\"y\":%d,\"z\":%d,\"a\":%d},\"armed\":\"%s\",\"remoteStart\":%s,\"countdown\":%d,\"screen\":%d,\"leds\":%u,"
		"\"fpga\":{\"x\":%d,\"y\":%d,\"z\":%d,\"a\":%d},\"steps\":{\"x\":%lld,\"y\":%lld,\"z\":%lld,\"a\":%lld},"
		"\"posChecks\":%u,\"posFaults\":%u,\"posErr\":[%d,%d,%d,%d],\"faultLine\":%d,\"busErrors\":%u,\"busRetries\":%u,\"busErrno\":%d,\"reversalsWaited\":%u,\"bursts\":%u,"
		"\"perf\":{\"loops\":%u,\"tickUs\":%u,\"tickUsMax\":%u,\"renderUs\":%u,\"renderUsMax\":%u,\"drawUs\":%u,\"presentUs\":%u,\"gapMsMax\":%u,\"motionMs\":%llu,\"lostMs\":%llu,\"replanIters\":%u},\"tool\":{\"loaded\":%d,\"changeTo\":%d,\"confirm\":%s},\"jobPart\":\"%s\",\"estimate\":{\"seconds\":%.0f,\"remaining\":%s,\"feed\":%d},\"inputs\":%u,\"pendant\":{\"raw\":%u,\"held\":%s,\"axis\":%d,\"mult\":%d,\"estop\":%s,\"wheel\":%d},\"message\":\"%s\",\"driverWarning\":\"%s\",\"macro\":\"%s\",\"backend\":\"%s\",\"frame\":%u}",
		STATE_NAMES[C.m], C.homed ? "true" : "false", pos[0], pos[1], pos[2], pos[3], wp[0], wp[1], wp[2], wp[3], 54 + WD.active,
		nm, C.prog.loaded ? "true" : "false", C.prog.nlines, ctl_cur_line() + 1,
		C.mo.feed_ov, rapid_override(C.mo.feed_ov), C.mo.spin_ov, C.mo.applied_pct, C.spindle ? "true" : "false", C.rpm, C.coolant,
		C.limit_axis >= 0 ? AXIS_CH[C.limit_axis] : '-', C.limit_dir, C.lim_dir[0], C.lim_dir[1], C.lim_dir[2], C.lim_dir[3], (const char *[]){ "run", "tryCut", "simulate" }[C.armed],
		SET.remote_start ? "true" : "false", C.countdown, U.screen, (unsigned)ui_leds(),
		C.fpos[0], C.fpos[1], C.fpos[2], C.fpos[3], (long long)C.mo.steps[0], (long long)C.mo.steps[1], (long long)C.mo.steps[2], (long long)C.mo.steps[3],
		C.pos_checks, C.pos_faults, C.pos_err[0], C.pos_err[1], C.pos_err[2], C.pos_err[3], C.fault_line, bus_errors, bus_retries, bus_last_errno, C.mo.deferred, C.fp.bursts, PERF.loops, PERF.tick_us_avg, PERF.tick_us_max, PERF.render_us_avg, PERF.render_us_max, PERF.draw_us_avg, PERF.present_us_avg, PERF.gap_ms_max,
		(unsigned long long)PERF.motion_ms, (unsigned long long)PERF.lost_ms, C.mo.replan_iters,
		WD.loaded_tool, C.m == M_TOOL || C.tc_state != TC_NONE ? C.tc_tool : 0, C.tc_confirm ? "true" : "false",
		(const char *[]){ "", "G54", "G55", "G56", "G57", "G58", "G59" }[C.job_idx >= 0 && C.job_idx < NWCS && (C.m == M_RUN || C.m == M_HOLD || C.m == M_TOOL) ? C.job_idx + 1 : 0],
		est_t_now, est_left ? "true" : "false", C.mo.feed_ov,
		(unsigned)C.inputs, (unsigned)C.pend.raw, C.pend.held ? "true" : "false", C.pend.axis, C.pend.mult, C.pend.estop ? "true" : "false", (int)U.wheel_total,
		msg, dw, mac, C.sim ? "sim" : "real", DSP ? DSP->frame_no : 0);
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
		if (d->api_ro) { resp_text(r, 403, "application/json", "{\"error\":\"read_only\",\"detail\":\"%s can only be changed on the panel\"}", d->key); return; }
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
	if (C.prog.loaded && f) snprintf(C.prog.path, sizeof C.prog.path, "%.250s", path);   /* it is a file now (restart memory) */
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

/* a growing JSON body for the list answers */
typedef struct { char *o; size_t k, cap; } jbuf_t;
static void jb(jbuf_t *j, const char *fmt, ...)
{
	va_list ap;
	for (;;) {
		va_start(ap, fmt);
		int n = vsnprintf(j->o + j->k, j->cap - j->k, fmt, ap);
		va_end(ap);
		if (n >= 0 && j->k + (size_t)n < j->cap) { j->k += (size_t)n; return; }
		j->cap *= 2; j->o = realloc(j->o, j->cap);
	}
}
static void jb_send(http_resp_t *r, jbuf_t *j) { r->body = j->o; r->len = j->k; r->status = 200; r->type = "application/json"; }

/* the value of ?name=... in a query string */
static bool qarg(const char *q, const char *name, char *out, size_t n)
{
	size_t l = strlen(name);
	for (const char *s = q; s && *s; ) {
		if (!strncmp(s, name, l) && s[l] == '=') {
			size_t i = 0; s += l + 1;
			while (*s && *s != '&' && i + 1 < n) out[i++] = *s++;
			out[i] = 0; return true;
		}
		s = strchr(s, '&'); if (s) s++;
	}
	return false;
}

static void get_offsets(http_resp_t *r)
{
	jbuf_t j = { malloc(1024), 0, 1024 };
	jb(&j, "{\"active\":\"G%d\",\"wcs\":{", 54 + WD.active);
	for (int i = 0; i < NWCS; i++)
		jb(&j, "%s\"G%d\":{\"x\":%.4f,\"y\":%.4f,\"z\":%.4f,\"a\":%.4f,\"job\":%s}", i ? "," : "", 54 + i,
			WD.wcs[i].off[0], WD.wcs[i].off[1], WD.wcs[i].off[2], WD.wcs[i].off[3], WD.wcs[i].job ? "true" : "false");
	jb(&j, "}}");
	jb_send(r, &j);
}

static void get_tools(http_resp_t *r)
{
	jbuf_t j = { malloc(1024), 0, 1024 };
	jb(&j, "{\"loaded\":%d,\"tools\":[", WD.loaded_tool);
	for (int i = 0; i < NTOOLS; i++) {
		char n[64]; json_escape(n, sizeof n, WD.tool[i].note);
		jb(&j, "%s{\"t\":%d,\"dia\":%.4f,\"len\":%.4f,\"note\":\"%s\"}", i ? "," : "", i + 1, WD.tool[i].dia, WD.tool[i].len, n);
	}
	jb(&j, "]}");
	jb_send(r, &j);
}

static int prog_ext(const char *n)
{
	const char *e = strrchr(n, '.');
	return e && (!strcasecmp(e, ".nc") || !strcasecmp(e, ".ngc") || !strcasecmp(e, ".tap") || !strcasecmp(e, ".gcode") || !strcasecmp(e, ".txt"));
}
static void add_prog(const char *name, void *ctx)
{
	jbuf_t *j = ctx;
	if (name[0] == '.' || !prog_ext(name)) return;
	char n[200]; json_escape(n, sizeof n, name);
	jb(j, "%s\"%s\"", j->o[j->k - 1] == '[' ? "" : ",", n);
}

/* GET programs?source=local|usb: the program files in /local (CNCDISK share) or on the USB stick */
static void get_programs(const http_req_t *q, http_resp_t *r)
{
	char src[16] = "local";
	qarg(q->query, "source", src, sizeof src);
	int s = !strcasecmp(src, "usb") ? 1 : 0;
	jbuf_t j = { malloc(2048), 0, 2048 };
	jb(&j, "{\"source\":\"%s\",\"dir\":\"%s\",\"files\":[", s ? "usb" : "local", UI_SRC_DIRS[s]);
	int n = dir_list(UI_SRC_DIRS[s], add_prog, &j);
	if (n < 0 && s == 1) dir_list(UI_SRC_DIRS[2], add_prog, &j);   /* the stick's other mount point */
	jb(&j, "]}");
	jb_send(r, &j);
}

/* GET program: what is loaded; GET program/lines?from=1&count=50: its text */
static void get_program(const http_req_t *q, http_resp_t *r, bool lines)
{
	jbuf_t j = { malloc(4096), 0, 4096 };
	char nm[300], er[260], wn[260];
	json_escape(nm, sizeof nm, C.prog.name); json_escape(er, sizeof er, C.prog.err); json_escape(wn, sizeof wn, C.prog.warn);
	if (!lines) {
		jb(&j, "{\"loaded\":%s,\"name\":\"%s\",\"lines\":%d,\"error\":\"%s\",\"warning\":\"%s\",\"estimateS\":%.0f,"
			"\"box\":{\"min\":[%.3f,%.3f,%.3f],\"max\":[%.3f,%.3f,%.3f]},\"tools\":[",
			C.prog.loaded ? "true" : "false", nm, C.prog.nlines, er, wn, C.prog.est_cum ? C.prog.est_s : -1.0,
			C.prog.lo[0], C.prog.lo[1], C.prog.lo[2], C.prog.hi[0], C.prog.hi[1], C.prog.hi[2]);
		for (int i = 0; i < C.prog.ntc && i < PROG_MAX_TC; i++) jb(&j, "%s{\"line\":%d,\"tool\":%d}", i ? "," : "", C.prog.tc_line[i] + 1, C.prog.tc_tool[i]);
		jb(&j, "],\"resumeLine\":%d,\"workDone\":%d,\"macro\":%s}", C.resume_line + 1, C.work_done + 1, C.macro_on ? "true" : "false");
	} else {
		char v[16];
		int from = qarg(q->query, "from", v, sizeof v) ? atoi(v) : 1, count = qarg(q->query, "count", v, sizeof v) ? atoi(v) : 50;
		from = CLAMP(from, 1, C.prog.nlines ? C.prog.nlines : 1); count = CLAMP(count, 1, 1000);
		jb(&j, "{\"name\":\"%s\",\"from\":%d,\"lines\":[", nm, from);
		for (int i = from - 1; i < from - 1 + count && i < C.prog.nlines; i++) {
			char t[260], e[600];
			program_line(&C.prog, i, t, sizeof t);
			json_escape(e, sizeof e, t);
			jb(&j, "%s\"%s\"", i == from - 1 ? "" : ",", e);
		}
		jb(&j, "]}");
	}
	jb_send(r, &j);
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
	if (!strcmp(p, "macros") && !post) {                 /* the macro files Sea Otter found */
		static macro_t ml[MACRO_MAX];
		int n = macro_scan(ml, MACRO_MAX);
		size_t cap = 16384, k = 0;
		char *o = malloc(cap);
		k += (size_t)snprintf(o + k, cap - k, "{\"armed\":\"%s\",\"macros\":[", C.macro_on ? C.macro_name : "");
		for (int i = 0; i < n && k < cap - 600; i++) {
			char nm[100], in[200], pa[300];
			json_escape(nm, sizeof nm, ml[i].name); json_escape(in, sizeof in, ml[i].info); json_escape(pa, sizeof pa, ml[i].path);
			k += (size_t)snprintf(o + k, cap - k, "%s{\"name\":\"%s\",\"info\":\"%s\",\"file\":\"%s\",\"needsHomed\":%s,\"source\":\"%s\"}",
				i ? "," : "", nm, in, pa, ml[i].needs_homed ? "true" : "false", ml[i].user ? "share" : "package");
		}
		k += (size_t)snprintf(o + k, cap - k, "]}");
		r->body = o; r->len = k; r->status = 200; r->type = "application/json";
		return;
	}
	if (!post && !del) {
		if (!strcmp(p, "offsets")) { get_offsets(r); return; }
		if (!strcmp(p, "tools")) { get_tools(r); return; }
		if (!strcmp(p, "programs")) { get_programs(q, r); return; }
		if (!strcmp(p, "program")) { get_program(q, r, false); return; }
		if (!strcmp(p, "program/lines")) { get_program(q, r, true); return; }
	}
	if (!strcmp(p, "settings")) { if (allowed(r, SET.api_settings, "Changing settings", "api.settings")) put_settings(b, r); return; }
	if (!post && !del) { resp_text(r, 404, "text/plain", "not found"); return; }

	arg_t a = { 0 };
	char s[64];
	double v;
	if (!strcmp(p, "key") && !C.sim && !allowed(r, SET.api_keys, "Panel keys", "api.keys")) return;
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
		if (ctl_busy() || C.m == M_LIMIT || C.m == M_TOOL || !motion_idle(&C.mo)) act(C_STOP, NULL, SRC_API);
		C.countdown = 0;
		resp_text(r, 200, "application/json", "{\"ok\":true,\"state\":\"%s\",\"fpgaAtStop\":{\"x\":%d,\"y\":%d,\"z\":%d,\"a\":%d}}",
			STATE_NAMES[C.m], at[0], at[1], at[2], at[3]);
		return;
	}
	if (!strcmp(p, "machine/drives")) { bool on = true; jbool(b, "on", &on); if (on && !allowed(r, SET.api_drives, "Switching the drives on", "api.drives")) return; doit(r, on ? C_DRIVES_ON : C_DRIVES_OFF, NULL); return; }
	/* everything below that moves, switches or changes something has its own permission;
	 * Pause and Stop never need one */
	{
		static const struct { const char *path; int prefix; size_t off; const char *what, *key; } P[] = {
			{ "machine/home", 0, offsetof(settings_t, api_moves), "Homing", "api.moves" },
			{ "move/gotozero", 0, offsetof(settings_t, api_moves), "Goto 0 / Safe Z", "api.moves" },
			{ "jog", 0, offsetof(settings_t, api_jog), "Jogging", "api.jog" },
			{ "machine/spindle", 0, offsetof(settings_t, api_spindle), "The spindle", "api.spindle" },
			{ "machine/coolant", 0, offsetof(settings_t, api_coolant), "Coolant", "api.coolant" },
			{ "offsets/", 1, offsetof(settings_t, api_offsets), "Work offsets", "api.offsets" },
			{ "tools/", 1, offsetof(settings_t, api_tools), "The tool table", "api.tools" },
			{ "settings/defaults", 0, offsetof(settings_t, api_settings), "Changing settings", "api.settings" },
			{ "program/", 1, offsetof(settings_t, api_programs), "Programs", "api.programs" },
			{ "run/arm", 0, offsetof(settings_t, api_programs), "Arming", "api.programs" },
			{ "run/rewind", 0, offsetof(settings_t, api_programs), "Rewinding", "api.programs" },
			{ "overrides", 0, offsetof(settings_t, api_overrides), "Overrides", "api.overrides" },
			{ "panel/", 1, offsetof(settings_t, api_panel), "Panel messages and screens", "api.panel" },
		};
		for (size_t i = 0; i < ARRAY_LEN(P); i++)
			if (P[i].prefix ? !strncmp(p, P[i].path, strlen(P[i].path)) : !strcmp(p, P[i].path))
				if (!allowed(r, *(const bool *)((const char *)&SET + P[i].off), P[i].what, P[i].key)) return;
	}
	if (!strcmp(p, "machine/home")) { doit(r, C_HOME, NULL); return; }
	if (!strcmp(p, "jog")) {
		if (!jstr(b, "axis", s, sizeof s) || (a.axis = axis_of(s)) < 0 || !jnum(b, "dist", &v)) { resp_text(r, 400, "application/json", "{\"error\":\"need axis and dist\"}"); return; }
		a.dir = v < 0 ? -1 : 1; a.dist = fabs(v);
		doit(r, C_JOG, &a); return;
	}
	if (!strcmp(p, "move/gotozero")) { bool z = false; jbool(b, "zOnly", &z); a.kind = z ? 1 : 0; doit(r, C_GOTOZERO, &a); return; }
	if (!strcmp(p, "machine/spindle")) {                 /* {"on":true,"rpm":12000} - by hand, like F6 */
		bool on = !C.spindle; jbool(b, "on", &on);
		if (jnum(b, "rpm", &v)) C.rpm = CLAMP(v, 0, SET.spindle_max);
		if (on != C.spindle) { doit(r, C_SPINDLE, NULL); return; }
		resp_text(r, 200, "application/json", "{\"ok\":true,\"state\":\"%s\",\"spindle\":%s,\"rpm\":%.0f}", STATE_NAMES[C.m], C.spindle ? "true" : "false", C.rpm);
		return;
	}
	if (!strcmp(p, "machine/coolant")) {                 /* {"on":true}: the M8 output */
		bool on = !(C.coolant & 1); jbool(b, "on", &on);
		if (on != (bool)(C.coolant & 1)) { doit(r, C_COOLANT, NULL); return; }
		resp_text(r, 200, "application/json", "{\"ok\":true,\"state\":\"%s\",\"coolant\":%d}", STATE_NAMES[C.m], C.coolant);
		return;
	}
	if (!strcmp(p, "move/macro")) {                    /* {"macro":"Park"} (name or file) -> armed and started */
		char mname[96];
		if (!jstr(b, "macro", mname, sizeof mname)) { resp_text(r, 400, "application/json", "{\"error\":\"need macro (its name or file)\"}"); return; }
		a.path = mname;
		if (can(C_MACRO, &a, SRC_API)) { refuse(r, C_MACRO, &a); return; }
		act(C_MACRO, &a, SRC_API);
		if (!C.macro_on) { char e[200]; json_escape(e, sizeof e, C.msg); resp_text(r, 400, "application/json", "{\"error\":\"macro\",\"detail\":\"%s\"}", e); return; }
		run_start(b, r);                                    /* same countdown / remote-START rules as a program */
		return;
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
	if (!strncmp(p, "tools/T", 7)) {                    /* {"dia":6,"len":58.33,"note":"6 mm flat"} */
		int t = atoi(p + 7);
		if (t < 1 || t > NTOOLS) { resp_text(r, 404, "application/json", "{\"error\":\"no such tool (T1-T%d)\"}", NTOOLS); return; }
		if (can(C_EDIT_TABLE, NULL, SRC_API)) { refuse(r, C_EDIT_TABLE, NULL); return; }
		tool_t *tl = &WD.tool[t - 1];
		if (jnum(b, "dia", &v)) tl->dia = CLAMP(v, 0, 999);
		if (jnum(b, "len", &v)) tl->len = CLAMP(v, 0, 999);
		char note[sizeof tl->note];
		if (jstr(b, "note", note, sizeof note)) snprintf(tl->note, sizeof tl->note, "%s", note);
		workdata_save(C.wd_path);
		resp_text(r, 200, "application/json", "{\"ok\":true}"); return;
	}
	if (!strcmp(p, "tools/loaded")) {                   /* {"tool":3}: the tool now in the spindle */
		if (!jnum(b, "tool", &v) || v < 0 || v > NTOOLS) { resp_text(r, 400, "application/json", "{\"error\":\"need tool 0-%d\"}", NTOOLS); return; }
		if (ctl_busy() || C.m == M_TOOL) { refuse(r, C_EDIT_TABLE, NULL); return; }
		WD.loaded_tool = (int)v; workdata_save(C.wd_path);
		resp_text(r, 200, "application/json", "{\"ok\":true,\"loaded\":%d}", WD.loaded_tool); return;
	}
	if (!strcmp(p, "settings/defaults")) {               /* the panel's Settings F2 */
		if (can(C_EDIT_SETTING, NULL, SRC_API) || ctl_busy()) { refuse(r, C_EDIT_SETTING, NULL); return; }
		bool api = SET.api; int port = SET.api_port;
		settings_defaults(&SET); SET.api = api; SET.api_port = port;   /* never lock the caller out */
		ctl_apply_axis_config(); settings_save(C.cfg_path);
		ctl_toast(false, "Settings restored to the defaults (over the network)");
		resp_text(r, 200, "application/json", "{\"ok\":true}"); return;
	}
	if (!strcmp(p, "run/arm")) {                         /* {"mode":"tryCut"}: what Start will do, without starting */
		char mode[16] = "";
		if (!jstr(b, "mode", mode, sizeof mode)) { resp_text(r, 400, "application/json", "{\"error\":\"need mode: run, tryCut or simulate\"}"); return; }
		if (ctl_busy() || C.m == M_TOOL) { refuse(r, C_START, NULL); return; }   /* like the panel's F5 */
		C.armed = !strcasecmp(mode, "tryCut") ? ARM_TRYCUT : !strcasecmp(mode, "simulate") ? ARM_SIM : ARM_RUN;
		resp_text(r, 200, "application/json", "{\"ok\":true,\"armed\":\"%s\"}", (const char *[]){ "run", "tryCut", "simulate" }[C.armed]); return;
	}
	if (!strcmp(p, "run/rewind")) {                      /* forget the resume point and the work done */
		if (ctl_busy() || C.m == M_TOOL) { refuse(r, C_START, NULL); return; }
		C.resume_line = -1; C.work_done = -1;
		resp_text(r, 200, "application/json", "{\"ok\":true}"); return;
	}
	if (!strcmp(p, "panel/screen")) {                     /* {"screen":"files"} */
		static const char *const names[SCR__N] = { "run", "jog", "files", "work", "tools", "settings", "macros", "io" };
		int sc = -1;
		if (jstr(b, "screen", s, sizeof s)) for (int i = 0; i < SCR__N; i++) if (!strcasecmp(s, names[i])) sc = i;
		if (sc < 0) { resp_text(r, 400, "application/json", "{\"error\":\"screen: run, jog, files, work, tools, settings, macros or io\"}"); return; }
		ui_goto((scr_t)sc); U.dirty = true;
		resp_text(r, 200, "application/json", "{\"ok\":true,\"screen\":\"%s\"}", names[U.screen]); return;
	}
	if (!strcmp(p, "program/upload")) { upload(q, r); return; }
	if (!strcmp(p, "program/load")) {
		char path[400], nm[160], src[16] = "local";
		if (!jstr(b, "name", nm, sizeof nm) || strchr(nm, '/')) { resp_text(r, 400, "application/json", "{\"error\":\"need a file name\"}"); return; }
		jstr(b, "source", src, sizeof src);                 /* "local" (CNCDISK share, default) or "usb" */
		int sd = !strcasecmp(src, "usb") ? 1 : 0;
		snprintf(path, sizeof path, "%s/%s", UI_SRC_DIRS[sd], nm);
		FILE *tf = fopen(path, "rb");
		if (!tf && sd == 1) snprintf(path, sizeof path, "%s/%s", UI_SRC_DIRS[2], nm);   /* the stick's other mount point */
		if (tf) fclose(tf);
		else if (!(tf = fopen(path, "rb"))) { resp_text(r, 404, "application/json", "{\"error\":\"no such file\",\"detail\":\"%s not found in %s\"}", nm, UI_SRC_DIRS[sd]); return; }
		else fclose(tf);
		a.path = path;
		if (can(C_LOAD, &a, SRC_API)) { refuse(r, C_LOAD, &a); return; }
		act(C_LOAD, &a, SRC_API);
		char e[200]; json_escape(e, sizeof e, C.prog.err);
		resp_text(r, C.prog.loaded ? 200 : 400, "application/json", "{\"ok\":%s,\"lines\":%d,\"detail\":\"%s\"}", C.prog.loaded ? "true" : "false", C.prog.nlines, C.prog.loaded ? "" : e); return;
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
