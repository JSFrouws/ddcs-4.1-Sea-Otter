/* Pandora - replacement operator app for the DDCS V4.1.
 *
 *   seaotter                on the controller: vendor /dev/motion, TM1638 keys, /dev/fb0
 *   seaotter --sim          simulated FPGA + machine (host, or on the unit without moving anything)
 * Options: --fb DEV | --no-fb, --port N, --data DIR, --programs DIR, --usb DIR
 * Browse to http://<host>:8080/ for the live panel (and the API under /api/v1/). */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "core/machine.h"
#include "core/macro.h"
#include "hal/display.h"
#include "hal/panel.h"
#include "net/api.h"
#include "sim/fpga_sim.h"
#include "ui/ui.h"
#include "perf.h"

extern const char *UI_SRC_DIRS[3];
static volatile sig_atomic_t quit;
static void on_sig(int s) { (void)s; quit = 1; }

/* cheap "did anything visible change" signature */
static uint64_t signature(void)
{
	uint64_t h = 1469598103934665603ull;
#define MIX(v) h = (h ^ (uint64_t)(v)) * 1099511628211ull
	if (!U.view.saver || !(C.m == M_RUN || C.mo.active)) {   /* CPU saver: position only via the 1 s tick */
		for (int a = 0; a < NAXES; a++) MIX(C.mo.steps[a]);
		MIX(C.mo.line);
	}
	MIX(C.m); MIX(C.msg_t); MIX(C.countdown); MIX(C.spindle); MIX(C.coolant); MIX(C.mo.feed_ov);
	MIX(C.mo.spin_ov); MIX(C.homed); MIX(C.inputs); MIX(C.pend.held); MIX(C.pend.axis); MIX(C.work_done); MIX(C.agent.on);
	MIX(now_ms() / 1000);                                  /* toasts expire, banners blink */
	for (int i = 0; i < C.agent.n; i++) MIX(C.agent.step[i].done);
	return h;
}

/* PANDORA_TRACE: bus accesses go to a RAM ring (no I/O while running, so timing is
 * unchanged) and are written out at exit */
#define TRN (1 << 20)
static struct { uint32_t t; char d; uint8_t a; uint16_t v; } *tr;
static uint32_t trn;
static void trace_cb(void *ctx, char d, uint8_t a, uint16_t v)
{
	if (a == 0x67 || (d == 'R' && a != 0x01 && (a < 0x02 || a > 0x09))) return;   /* keep it small */
	uint32_t i = trn++ % TRN;
	tr[i].t = (uint32_t)now_ms(); tr[i].d = d; tr[i].a = a; tr[i].v = v;
}
static void trace_dump(FILE *f)
{
	uint32_t first = trn > TRN ? trn - TRN : 0;
	for (uint32_t k = first; k < trn; k++) { uint32_t i = k % TRN; fprintf(f, "%u %c %02x %04x\n", tr[i].t, tr[i].d, tr[i].a, tr[i].v); }
	fclose(f);
}

/* --bench-render PROGRAM: time the pieces of a frame on this CPU (no motion, nothing moves) */
void dro_row(fb_t *fb, int x, int y, int w, int a, const font_t *big);
static int bench_render(display_t *d, const char *prog)
{
	arg_t a = { .path = prog };
	act(C_LOAD, &a, SRC_PANEL);
	fb_t *fb = &d->back;
	const int N = 10;
	uint64_t t;
	#define BENCH(name, code) do { t = now_us(); for (int i_ = 0; i_ < N; i_++) { code; } printf("%-34s %7.2f ms\n", name, (now_us() - t) / 1000.0 / N); } while (0)
	scr_t scr[] = { SCR_RUN, SCR_JOG, SCR_FILES, SCR_SETTINGS };
	const char *nm[] = { "full render: Run", "full render: Jog", "full render: Files", "full render: Settings" };
	for (int s = 0; s < 4; s++) { U.screen = scr[s]; BENCH(nm[s], ui_render(fb)); }
	U.screen = SCR_RUN;
	gfx_noclip(fb);
	BENCH("background gradient (full screen)", { gfx_vgrad(fb, 0, 0, SCR_W, 140, COL_BG0, COL_BG1); gfx_fill(fb, 0, 140, SCR_W, SCR_H - 140, COL_BG1); });
	BENCH("plain fill (full screen)", gfx_fill(fb, 0, 0, SCR_W, SCR_H, COL_BG1));
	BENCH("one pane (gfx_panel 600x300)", gfx_panel(fb, 100, 100, 600, 300, false));
	BENCH("toolpath view (Run size)", ui_view_render(fb, 300, 60, 594, 300, false));
	U.view.iso = true;
	BENCH("toolpath view ISO", ui_view_render(fb, 300, 60, 594, 300, false));
	U.view.iso = false;
	BENCH("4 DRO rows (font_28)", for (int k = 0; k < 4; k++) dro_row(fb, 8, 80 + k * 42, 290, k, &font_28));
	BENCH("text: 40 chars font_14", gfx_text(fb, &font_14, 10, 10, COL_TXT, "The quick brown fox jumps over the lazy dog"));
	sk_t sk[7] = { { "Start", "run" }, { "Pause", "x" }, { "Reset", "x" }, { "View", "x" }, { "TryCut", "x" }, { "Spindle", "x" }, { "Page R", "x" } };
	BENCH("frame (top bar, soft keys, strip)", ui_frame(fb, sk));
	memset(d->front, 0, SCR_W * SCR_H * 2);
	BENCH("present: every row changed", { memset(d->front, (i_ & 1) ? 0 : 0xFF, SCR_W * SCR_H * 2); display_present(d); });
	BENCH("present: nothing changed", display_present(d));
	printf("preview vertices: %d\n", C.prog.npv);
	return 0;
}

/* One motion slice covering the real elapsed time (back-to-back catch-up slices would hit axes
 * that are still stepping), clamped to 40 ms: the longest burst = the shortest stop. */
static bool g_sim;
static uint64_t g_next, g_last_tick;
static void motion_slice(void)
{
	uint64_t t = now_ms();
	uint32_t real = g_last_tick ? (uint32_t)(t - g_last_tick) : TICK_MS;
	uint32_t el = CLAMP(real, TICK_MS, 40u);
	PERF.loops++;
	if (real > PERF.gap_ms_max) PERF.gap_ms_max = real;
	if (C.m == M_RUN || C.mo.active) { PERF.motion_ms += el; if (real > el) PERF.lost_ms += real - el; }
	uint64_t t_us = now_us();
	g_last_tick = t;
	g_next = t + TICK_MS;
	ctl_tick(el / 1000.0);
	if (g_sim) sim_step(el);
	uint32_t tk = (uint32_t)(now_us() - t_us);
	PERF.tick_us_avg = (PERF.tick_us_avg * 15 + tk) / 16;
	if (tk > PERF.tick_us_max) PERF.tick_us_max = tk;
}
static void slice_if_due(void) { if (now_ms() >= g_next) motion_slice(); }


int main(int argc, char **argv)
{
	bool sim = false, nofb = false;
	const char *bench_prog = NULL;
	const char *fbdev = "/dev/fb0", *data = NULL, *progs = NULL, *usb = NULL;
	int port = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--sim")) sim = true;
		else if (!strcmp(argv[i], "--bench-render") && i + 1 < argc) bench_prog = argv[++i];
		else if (!strcmp(argv[i], "--no-fb")) nofb = true;
		else if (!strcmp(argv[i], "--fb") && i + 1 < argc) fbdev = argv[++i];
		else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
		else if (!strcmp(argv[i], "--programs") && i + 1 < argc) progs = argv[++i];
		else if (!strcmp(argv[i], "--usb") && i + 1 < argc) usb = argv[++i];
		else { fprintf(stderr, "usage: %s [--sim] [--fb DEV|--no-fb] [--port N] [--data DIR] [--programs DIR] [--usb DIR]\n", argv[0]); return 2; }
	}
	bus_t *bus = sim ? NULL : bus_ioctl_open("/dev/motion");
	if (!bus) {
		if (!sim) fprintf(stderr, "/dev/motion not available (vendor app still running, or not a DDCS) - using the simulator\n");
		sim = true;
		sim_reset_machine();
		bus = bus_sim_open();
	}
	/* the real keypad is only ever touched on the ARM target: it maps physical memory, which
	 * on any other machine would be somebody else's registers. With --sim on the unit this
	 * gives real keys + real LCD + a simulated machine - the safe first test on hardware. */
	panel_t *panel = NULL;
#ifdef __arm__
	panel = panel_tm1638_open();
#endif
	if (!panel) panel = panel_sim_open();
	struct stat st;
	if (!data) data = stat("/ddcsv4", &st) == 0 && !sim ? "/ddcsv4" : ".";
	if (progs) UI_SRC_DIRS[0] = progs;
	else if (stat("/local", &st) != 0) UI_SRC_DIRS[0] = "programs";
	/* macros: the package's macro-*.nc in the app folder, and the operator's own in
	 * <programs>/macros (= the CNCDISK share on the controller). On a PC without an app
	 * folder of its own the package sources are used. */
	snprintf(macro_dir_sys, sizeof macro_dir_sys, "%s", data);
	if (strcmp(data, "/ddcsv4") && stat("install/usb/ddcsv4", &st) == 0) snprintf(macro_dir_sys, sizeof macro_dir_sys, "install/usb/ddcsv4");
	snprintf(macro_dir_user, sizeof macro_dir_user, "%s/macros", UI_SRC_DIRS[0]);
	if (usb) UI_SRC_DIRS[1] = usb;
	char cfg[300], wd[300];
	snprintf(cfg, sizeof cfg, "%s/seaotter.cfg", data);
	snprintf(wd, sizeof wd, "%s/seaotter-work.cfg", data);

	/* PANDORA_TRACE=file: log every FPGA register access (diagnostics on the real unit) */
	static FILE *trace_f;
	if (getenv("PANDORA_TRACE") && (trace_f = fopen(getenv("PANDORA_TRACE"), "w"))) {
		tr = calloc(TRN, sizeof *tr);
		bus->trace = trace_cb; bus->trace_ctx = trace_f;
	}
	display_t dsp;
	display_open(&dsp, nofb || (sim && stat(fbdev, &st) != 0) ? NULL : fbdev);
	/* boot logo (data dir/logo.bmp, as the vendor app does): shown while starting, at least
	 * 2 s, a key press ends it early */
	char logo[300];
	snprintf(logo, sizeof logo, "%s/logo.bmp", data);
	uint64_t splash_until = display_logo(&dsp, logo) ? now_ms() + 2000 : 0;
	ctl_init(bus, sim, cfg, wd);
	ui_init();
	{                                                   /* written by the start script at boot */
		FILE *f = fopen("/tmp/seaotter-driver.txt", "r");
		char b[200] = "";
		if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); }
		b[strcspn(b, "\r\n")] = 0;
		if (!strncmp(b, "WARNING", 7)) {                 /* a permanent banner, not a toast */
			snprintf(C.driver_warn, sizeof C.driver_warn, "%.190s", b + 8);
			ctl_toast(true, "%s", b + 8);
		}
	}
	if (bench_prog) return bench_render(&dsp, bench_prog);
	api_bind(&dsp, panel);
	if (!port) port = SET.api_port;
	if (http_init(port, api_handle) < 0) fprintf(stderr, "HTTP port %d unavailable - API and browser panel off\n", port);
	else fprintf(stderr, "Sea Otter %s (%s): %s backend, panel at http://localhost:%d/\n", BUILD_ID, BUILD_DATE, sim ? "SIMULATED" : "real", port);

	signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN);
	g_sim = sim;
	ui_yield = slice_if_due;
	uint64_t last_scan = 0, last_render = 0, last_save = now_ms(), sig = 0;
	uint32_t keys_prev = 0, keys_cand = 0;
	while (!quit) {
		uint64_t t = now_ms();
		if (t >= g_next) {
			motion_slice();
			int counts = fpga_wheel_delta(&C.fp);
			ui_pendant(&C.pend, counts);
			if (t - last_scan >= 20) {                          /* keys: 50 Hz, debounced over two scans */
				last_scan = t;
				uint32_t k = panel->scan(panel) | panel->sim_keys;   /* + keys from the browser panel */
				if (k == keys_cand && k != keys_prev) {
					uint32_t down = k & ~keys_prev, up = keys_prev & ~k;
					for (int b = 0; b < 32; b++) {
						int key = panel_bit_to_key(b);
						if (!key) continue;
						if (down & (1u << b)) ui_key((pkey_t)key);
						if (up & (1u << b)) ui_key_release((pkey_t)key);
					}
					keys_prev = k;
				}
				keys_cand = k;
				panel->leds(panel, ui_leds());
			}
			uint64_t s = signature();
			bool mv = C.m == M_RUN || C.mo.active;
			uint64_t gap = !mv ? 60 : U.view.saver && !U.dirty ? 1000 : 200;   /* <= 5 fps moving; CPU saver 1 fps unless a key was pressed */
			if (splash_until && (t >= splash_until || keys_prev)) { splash_until = 0; U.dirty = true; }
			if (!splash_until && (U.dirty || s != sig) && t - last_render >= gap) {
				uint64_t r_us = now_us();
				ui_render(&dsp.back);
				slice_if_due();                                   /* a frame is ~60 ms here: keep motion */
				uint64_t p_us = now_us();                         /* ticking between its parts so no gap */
				for (int band = 0; band < 4; band++) {            /* exceeds one 40 ms motion slice */
					display_present_rows(&dsp, band * (SCR_H / 4), SCR_H / 4);
					slice_if_due();
				}
				dsp.frame_no++;
				uint32_t rd = (uint32_t)(now_us() - r_us);
				PERF.draw_us_avg = (PERF.draw_us_avg * 7 + (uint32_t)(p_us - r_us)) / 8;
				PERF.present_us_avg = (PERF.present_us_avg * 7 + (uint32_t)(now_us() - p_us)) / 8;
				PERF.render_us_avg = (PERF.render_us_avg * 7 + rd) / 8;
				if (rd > PERF.render_us_max) PERF.render_us_max = rd;
				last_render = t; sig = s;
			}
			if (t - last_save > 30000 && (C.m == M_IDLE || C.m == M_LIMIT || C.m == M_OFF)) { ctl_save(); last_save = t; }
		}
		int wait = (int)(g_next > now_ms() ? g_next - now_ms() : 0);
		http_poll(wait);
	}
	ctl_save();
	if (trace_f) trace_dump(trace_f);
	fpga_set_outputs(&C.fp, 0);
	fpga_spindle_pwm(&C.fp, 0);
	panel->leds(panel, 0);
	fprintf(stderr, "seaotter: saved, outputs off, bye\n");
	return 0;
}
