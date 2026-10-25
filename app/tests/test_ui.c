/* UI against the controller: key routing, screens, editor, pendant; every screen is
 * rendered headless and written to build/snapshots/NN_name.ppm for a visual check. */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "t.h"
#include "ui/ui.h"
#include "sim/fpga_sim.h"
#include "hal/display.h"

static fb_t fb;

static void snap(const char *name)
{
	ui_render(&fb);
	mkdir("snapshots", 0755);
	char path[128];
	snprintf(path, sizeof path, "snapshots/%s.ppm", name);
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", SCR_W, SCR_H);
	for (int i = 0; i < SCR_W * SCR_H; i++) {
		uint16_t v = fb.px[i];
		unsigned char c[3] = { (unsigned char)((v >> 8) & 0xF8), (unsigned char)((v >> 3) & 0xFC), (unsigned char)((v << 3) & 0xF8) };
		fwrite(c, 1, 3, f);
	}
	fclose(f);
}

static void keys(const pkey_t *k, int n) { for (int i = 0; i < n; i++) ui_key(k[i]); }

void test_ui(void)
{
	/* the boot logo: the committed placeholder loads (1024 x 600, 24-bit BMP, the vendor's
	 * format), anything else is refused and leaves the screen alone */
	{
		display_t d;
		display_open(&d, NULL);
		CHECK(display_logo(&d, "../install/usb/ddcsv4/logo.bmp"));
		int colours = 0;                                         /* a picture, not a blank screen */
		for (int i = 1; i < SCR_W * SCR_H && colours < 50; i += 997) colours += d.front[i] != d.front[i - 1];
		CHECK(colours >= 50);
		CHECK(d.frame_no == 1);
		display_logo_caption(&d, "Starting…", "Sea Otter test");   /* the start script's splash */
		CHECK(d.frame_no == 2);
		{
			mkdir("snapshots", 0755);
			FILE *sf = fopen("snapshots/00a_boot_splash.ppm", "wb");
			if (sf) {
				fprintf(sf, "P6\n%d %d\n255\n", SCR_W, SCR_H);
				for (int i = 0; i < SCR_W * SCR_H; i++) {
					uint16_t v = d.back.px[i];
					unsigned char c[3] = { (unsigned char)((v >> 8) & 0xF8), (unsigned char)((v >> 3) & 0xFC), (unsigned char)((v << 3) & 0xF8) };
					fwrite(c, 1, 3, sf);
				}
				fclose(sf);
			}
		}
		FILE *f = fopen("not_a_logo.bmp", "wb");
		if (f) { fputs("BM this is not a 1024x600 bitmap", f); fclose(f); }
		CHECK(!display_logo(&d, "not_a_logo.bmp"));
		CHECK(!display_logo(&d, "missing.bmp"));
		CHECK(d.frame_no == 2);
		remove("not_a_logo.bmp");
		free(d.back.px); free(d.front);
	}

	SUITE("ui");
	fx_reset();
	SET.spindle_delay = 0;
	ui_init();
	fb.w = SCR_W; fb.h = SCR_H; fb.px = calloc(SCR_W * SCR_H, 2);
	const char *prog = "(demo part)\nG21 G90 G54\nG0 Z5\nG0 X0 Y0\nM3 S12000\nG1 Z-2 F300\nG1 X40 F900\nG2 X60 Y20 I0 J20\nG1 Y40\nG3 X40 Y60 I-20 J0\n"
		"G1 X0\nG1 Y0\nG0 Z5\nG81 X70 Y10 Z-6 R2 F200\nX70 Y30\nX70 Y50\nG80\nM5\nM30\n";
	/* no program: the listing says so, Enter doesn't invent a resume line */
	U.view.iso = true; snap("00_run_no_program");
	ui_key(K_TAB); ui_key(K_ENTER);
	CHECK(C.resume_line < 0 && strstr(C.msg, "No program"));
	ui_key(K_TAB); U.view.iso = false;
	arg_t a = { .path = "demo.nc", .text = prog, .len = strlen(prog) };
	act(C_LOAD, &a, SRC_PANEL);
	snap("01_run_idle");

	/* Mode -> Jog, the side strip and the touch-off ALT layer */
	ui_key(K_MODE);
	CHECK(U.screen == SCR_JOG);
	SET.edge_dia = 12;
	ui_key(K_SHIFT);
	snap("02_jog_alt_touchoff");
	ui_key(K_XP);                                    /* touch-off X+ -> -6, Shift stays lit */
	CHECK_NEAR(ctl_work(AX_X), -6, 1e-9);
	CHECK(U.shift);
	ui_key(K_XP);                                    /* same key again -> 0 */
	CHECK_NEAR(ctl_work(AX_X), 0, 1e-9);
	CHECK(!U.shift);

	/* the jog setup pane + digit editor: 0.00, two left, Y+ -> 100.00; Y- would give 900 */
	ui_key(K_TAB);
	CHECK(ui_pane() == 1);
	U.jog_field = 0;
	SET.edge_dia = 0;
	ui_key(K_ENTER);
	CHECK(U.ed.on);
	pkey_t e1[] = { K_XM, K_XM, K_YP };
	keys(e1, 3);
	snap("03_digit_editor");
	ui_key(K_ENTER);
	CHECK_NEAR(SET.edge_dia, 50, 1e-9);              /* clamped to the setting's max (50) */
	ui_key(K_ENTER);
	pkey_t e2[] = { K_XM, K_YM, K_ENTER };            /* 50.00 -> tens digit 5-1 = 4 -> 40.00 */
	keys(e2, 3);
	CHECK_NEAR(SET.edge_dia, 40, 1e-9);
	/* a value that may be negative: X- past a leading zero reaches the sign, Modify flips it */
	{
		static double got; got = 0;
		void cb(double v, int c) { (void)c; got = v; }
		ui_editor_open("WCS test", 12.345, 3, "mm", 3, 99999, true, false, cb, 0);
		snap("03b_editor_greyed_keys");
		pkey_t s1[] = { K_XM, K_XM, K_XM, K_YP, K_ENTER };      /* cursor 2 -> 1 -> 0 (a '0') -> sign; flip */
		keys(s1, 5);
		CHECK_NEAR(got, -12.345, 1e-9);
		ui_editor_open("WCS test", -5, 3, "mm", 3, 99999, true, false, cb, 0);
		ui_key(K_MOD); ui_key(K_ENTER);
		CHECK_NEAR(got, 5, 1e-9);
		ui_editor_open("Edge", 3, 2, "mm", 2, 50, false, false, cb, 0);
		ui_key(K_MOD); ui_key(K_ENTER);
		CHECK_NEAR(got, 3, 1e-9);                                  /* no sign where it can't be negative */
	}
	ui_key(K_TAB);

	/* jog keys move the machine; Mode back to Run; start -> Mode refuses Jog */
	double y0 = C.mo.pos[AX_Y];
	ui_key(K_YP);
	fx_until_idle(5000);
	CHECK_NEAR(C.mo.pos[AX_Y], y0 + 1, 1e-9);
	ui_key(K_MODE);
	CHECK(U.screen == SCR_RUN);
	ui_key(K_F1);
	CHECK(C.m == M_RUN);
	fx_run(1500);
	snap("04_run_running");
	ui_key(K_MODE);
	CHECK(U.screen == SCR_RUN);                      /* jog locked while running */
	ui_key(K_F7);                                    /* the screen menu: still usable while running */
	CHECK(U.pagesel && U.screen == SCR_RUN);
	snap("04b_page_menu_running");
	ui_key(K_F4);                                    /* Files sits above View */
	CHECK(U.screen == SCR_FILES && !U.pagesel);
	ui_key(K_YP);                                    /* nothing directional happens */
	CHECK(C.m == M_RUN);
	ui_key(K_F7); ui_key(K_F2);                      /* F2 = Pause on every screen while moving, menu or not */
	CHECK(C.m == M_HOLD && U.screen == SCR_FILES);
	ui_key(K_F3);
	CHECK(C.m == M_IDLE);
	fx_until_idle(5000);                             /* Reset decelerates before it flushes */
	snap("05_files");

	/* the screen menu: F1 Run, F2 Macros, F3 Tools, F4 Files, F5 Work, F6 Settings */
	ui_key(K_F7); CHECK(U.pagesel);
	snap("05b_page_menu");
	ui_key(K_F7); CHECK(!U.pagesel && U.screen == SCR_FILES);       /* Page R again closes it */
	ui_key(K_F7); ui_key(K_YP); CHECK(!U.pagesel && U.screen == SCR_FILES);   /* any key closes it */
	ui_key(K_F7); ui_key(K_F5); CHECK(U.screen == SCR_WORK); snap("06_work");
	ui_key(K_F7); ui_key(K_F3); CHECK(U.screen == SCR_TOOLS); snap("07_tools");
	ui_key(K_SHIFT); ui_key(K_F7); CHECK(U.screen == SCR_WORK);     /* Shift + Page R = last screen */
	ui_key(K_SHIFT); ui_key(K_F7); CHECK(U.screen == SCR_TOOLS);
	ui_key(K_F7); ui_key(K_F2); CHECK(U.screen == SCR_MACROS); snap("07b_macros");
	{                                                 /* Enter arms the macro and goes to Run */
		char keep[128]; snprintf(keep, sizeof keep, "%s", C.prog.name);
		U.macro = 0; ui_key(K_ENTER);
		CHECK(U.screen == SCR_RUN && C.macro_on && ui_banner_h() > 0);
		snap("07c_macro_armed");
		ui_key(K_F3);                                 /* Reset cancels: the program is back */
		CHECK(!C.macro_on && !strcmp(C.prog.name, keep));
		U.screen = SCR_MACROS;
	}
	ui_key(K_F7); ui_key(K_F6); CHECK(U.screen == SCR_SETTINGS); snap("08_settings");
	ui_key(K_F6); CHECK(U.screen == SCR_IO); snap("08f_io");      /* Settings F6: the I/O overview */
	ui_key(K_F6); CHECK(U.screen == SCR_SETTINGS);
	for (int g = 0; SETTING_GROUPS[g]; g++) if (!strcmp(SETTING_GROUPS[g], "API")) U.grp = g;
	U.pane[SCR_SETTINGS] = 1; U.item = 0; snap("08g_settings_api");
	for (int i = 0; i < NSETTINGS; i++) {                           /* F5: every setting has a hint */
		const char *h = setting_hint(SETTINGS[i].key);
		if (!h) printf("  no hint for %s\n", SETTINGS[i].key);
		CHECK(h && strlen(h) > 20);
	}
	ui_key(K_F5); U.item = 1; snap("08h_settings_hint");
	for (int i = 0; i < 40; i++) ui_key(K_YM);                     /* to the bottom: the back line clears the notes */
	snap("08i_settings_api_bottom");
	ui_key(K_F5); U.pane[SCR_SETTINGS] = 0; U.grp = 0;
	ui_key(K_F7); ui_key(K_F1); CHECK(U.screen == SCR_RUN);
	/* Shift: second functions on F1-F6 (Run) */
	ui_key(K_SHIFT); snap("08c_run_shift");
	{
		int w0 = WD.active;
		ui_key(K_F2); CHECK(U.wcssel && !U.shift);                      /* Shift + F2: F1-F6 = G54-G59 */
		snap("08k_wcs_menu");
		ui_key(K_F3); CHECK(WD.active == 2 && !U.wcssel);                /* F3 = G56 */
		U.pane[SCR_RUN] = 0; ui_key(K_SEL); CHECK(U.wcssel);             /* A+ opens it too */
		ui_key(K_CANCEL); CHECK(!U.wcssel && WD.active == 2);
		/* the drawing follows: a program switching to G55 itself, then another active offset */
		{
			wcs_t k0 = WD.wcs[0], k1 = WD.wcs[1];
			WD.active = 0; WD.wcs[1] = WD.wcs[0]; WD.wcs[1].off[AX_X] += 60; WD.wcs[1].job = false;
			const char *wp = "G21 G90 G54\nG0 Z5\nG0 X0 Y0\nG1 Z-1 F300\nG1 X20\nG0 Z5\nG55\nG0 X0 Y0\nG1 Z-1\nG1 Y20\nG0 Z5\nM30\n";
			arg_t wl = { .path = "two-wcs.nc", .text = wp, .len = strlen(wp) };
			act(C_LOAD, &wl, SRC_PANEL);
			CHECK(C.prog.pv_wcs0 == 0);
			U.view.iso = false; snap("08l_program_g55_inside");
			WD.active = 1; snap("08m_same_active_g55");        /* the same picture, seen from G55 */
			WD.wcs[0] = k0; WD.wcs[1] = k1;
		}
		WD.active = w0;
		/* Shift + F5: go to a line, with the program's sections / retracts listed beside it */
		const char *sp = "(header)\nG21 G90\n(Op 1 - face)\nG0 Z10\nG0 X0 Y0\nG1 Z-1 F300\nG1 X20\nG0 Z10\n(Op 2 - pocket)\nG0 X30\nG1 Z-2\nG1 Y20\nG0 Z10\nM30\n";
		char keep[128]; snprintf(keep, sizeof keep, "%s", C.prog.name);
		arg_t sl = { .path = "sections.nc", .text = sp, .len = strlen(sp) };
		act(C_LOAD, &sl, SRC_PANEL);
		CHECK(C.prog.goto_sections && C.prog.ngoto == 2 && C.prog.goto_line[0] == 2 && C.prog.goto_line[1] == 8);
		U.prog_line = 8;
		ui_key(K_SHIFT); ui_key(K_F5);
		CHECK(U.ed.on && U.goto_list);
		{                                                  /* user 2026-10-25: X picked on the pendant while the editor is up: */
			double x0 = C.mo.pos[AX_X];                    /* the wheel edits the number, nothing moves */
			pendant_t q = { .held = true, .axis = AX_X, .mult = 1 };
			char before[12]; memcpy(before, U.ed.d, sizeof before);
			ui_pendant(&q, 0); ui_pendant(&q, -8);
			fx_run(300);
			CHECK_NEAR(C.mo.pos[AX_X], x0, 1e-9);
			CHECK(memcmp(before, U.ed.d, sizeof before) != 0 && ui_pend_axis() == -1);
			q.held = false; ui_pendant(&q, 0);
			memcpy(U.ed.d, before, sizeof before);
		}
		snap("08d_goto_line");
		ui_key(K_ENTER);                                   /* line 9 = "(Op 2 - pocket)" */
		CHECK(!U.ed.on && C.resume_line == 8);
		/* not running: the lines before the picked one turn orange, a dot marks where it leads */
		CHECK(ui_pick_line() == 8);
		U.pane[SCR_RUN] = 0; CHECK(ui_pick_line() == 8);              /* the resume point, pane or not */
		U.view.iso = true; snap("08j_pick_orange_dot"); U.view.iso = false; U.pane[SCR_RUN] = 1;
		/* back to line 1 only with a second Reset */
		ui_key(K_F3); CHECK(C.resume_line == 8 && C.rewind_ask);
		snap("08e_rewind_asks");
		ui_key(K_YP); CHECK(C.resume_line == 8 && !C.rewind_ask);   /* any other key: stays */
		ui_key(K_F3); ui_key(K_F3); CHECK(C.resume_line == -1 && C.work_done == -1);
		/* without sections: the retracts to the most used safe height */
		const char *rp = "G21 G90\nG0 Z10\nG0 X0 Y0\nG1 Z-1 F300\nG1 X20\nG0 Z10\nG0 X30\nG1 Z-2\nG0 Z10\nG0 X40\nG1 Z-1\nG0 Z3\nM30\n";
		arg_t rl = { .path = "retracts.nc", .text = rp, .len = strlen(rp) };
		act(C_LOAD, &rl, SRC_PANEL);
		CHECK(!C.prog.goto_sections && C.prog.ngoto == 3 && C.prog.goto_line[0] == 1 && C.prog.goto_line[1] == 5 && C.prog.goto_line[2] == 8 && fabsf(C.prog.goto_z - 10) < 1e-4);
		arg_t back = { .path = "demo.nc", .text = prog, .len = strlen(prog) };
		act(C_LOAD, &back, SRC_PANEL);
		(void)keep;
		U.view.zoom = 3; ui_key(K_SHIFT); ui_key(K_F4); CHECK(U.view.zoom == 1);
		U.pane[SCR_RUN] = 0;
	}
	U.view.iso = U.view.saver = false;
	ui_key(K_F4);
	CHECK(U.view.iso && !U.view.saver);
	for (int i = 0; i < 20; i++) { ui_key(K_XP); ui_key(K_YM); }
	CHECK(U.view.yaw == 0 && U.view.pitch == 0);      /* X-Z side view, and no further */
	snap("09_iso_xz_limit");
	for (int i = 0; i < 20; i++) ui_key(K_XM);
	CHECK(U.view.yaw == -90);
	for (int i = 0; i < 20; i++) ui_key(K_YP);
	CHECK(U.view.pitch == 90);
	U.view.yaw = -30; U.view.pitch = 32;
	/* F4 cycles 2D -> ISO -> CPU saver -> 2D; an E-stop or limit brings ISO back */
	ui_key(K_F4); CHECK(U.view.saver && !U.view.iso); snap("09b_cpu_saver");
	ui_key(K_F4); CHECK(!U.view.saver && !U.view.iso);
	ui_key(K_F4); ui_key(K_F4); CHECK(U.view.saver);
	ui_render(&fb); C.m = M_ESTOP; ui_render(&fb); C.m = M_IDLE; ui_render(&fb);
	CHECK(!U.view.saver && U.view.iso);
	snap("10_iso");

	/* settings: decimals change what the DRO shows */
	U.screen = SCR_SETTINGS; U.pane[SCR_SETTINGS] = 1; U.grp = 0; U.item = 0;
	ui_key(K_XP);                                    /* nothing happens until editing starts */
	CHECK(SET.pos_dec == 3);
	ui_key(K_MOD);                                   /* start inline edit (= a pendant quick press) */
	ui_key(K_XP);                                    /* 0.000 -> 0.0 (cycles), not applied yet */
	CHECK(SET.pos_dec == 3);
	ui_key(K_ENTER);                                 /* accept */
	CHECK(SET.pos_dec == 1);
	ui_key(K_MOD); ui_key(K_XP); ui_key(K_CANCEL);   /* Cancel discards */
	CHECK(SET.pos_dec == 1);
	/* past either end: an orange "back to the groups" position that does NOT leave the group
	 * by itself - one step back returns to the list, Enter there goes to the categories */
	U.item = 0; ui_key(K_YP);
	CHECK(ui_pane() == 1 && U.item == -1);
	snap("08a_settings_back_top");
	ui_key(K_YP); CHECK(ui_pane() == 1 && U.item == -1);            /* stays, doesn't wrap */
	ui_key(K_YM); CHECK(ui_pane() == 1 && U.item == 0);             /* back into the list */
	ui_key(K_YP); ui_key(K_ENTER);
	CHECK(ui_pane() == 0);
	ui_key(K_ENTER); U.item = 0;
	int last = -1;
	for (int i = 0; i < 40; i++) { ui_key(K_YM); if (U.item == last) break; last = U.item; }
	CHECK(ui_pane() == 1 && U.item > 0);                            /* scrolled to the bottom, still in the group */
	snap("08b_settings_back_bottom");
	ui_key(K_YP); CHECK(ui_pane() == 1 && U.item == last - 1);      /* the last real item again */
	ui_key(K_YM); ui_key(K_SEL);                                    /* a quick press on the back line */
	CHECK(ui_pane() == 0);
	char b[32]; fmt_pos(b, sizeof b, -12.3456);
	CHECK(!strcmp(b, "−12.3"));
	SET.pos_dec = 3;

	/* pendant: wheel without hold navigates, hold+wheel changes, a quick press is Enter */
	U.screen = SCR_SETTINGS; U.pane[SCR_SETTINGS] = 0; U.grp = 0;
	pendant_t pd = { .axis = -1, .mult = 1 };
	/* raw counts as the real pendant gives them: clockwise (numbers up) counts DOWN */
	ui_pendant(&pd, -4);                             /* one detent clockwise = next item */
	CHECK(U.grp == 1);
	pd.held = true;
	ui_pendant(&pd, 0);
	pd.held = false;
	ui_pendant(&pd, 0);                              /* quick press -> Enter -> values pane */
	CHECK(ui_pane() == 1);
	U.screen = SCR_RUN; U.pane[SCR_RUN] = 1;
	act(C_START, NULL, SRC_PANEL);
	pd.held = true; pd.mult = 1;
	ui_pendant(&pd, 8);                              /* running: hold + wheel counter-clockwise = feed - */
	CHECK(C.mo.feed_ov == 96 && rapid_override(C.mo.feed_ov) == 96);   /* 2 % per detent */
	pd.axis = 0;
	double x0 = C.mo.pos[AX_X];
	ui_pendant(&pd, 40);                             /* X selected while running: refused */
	fx_run(50);
	CHECK(C.m == M_RUN);
	(void)x0;
	act(C_STOP, NULL, SRC_PANEL);
	fx_until_idle(5000);
	pd.held = false; pd.axis = -1;
	ui_pendant(&pd, 0);

	/* jog screen, step "continuous": the wheel never presses a jog key (it once started a Y
	 * jog that never stopped), the axis switch picks the pane, the picked axis gets a diamond */
	{
		U.screen = SCR_JOG; U.pane[SCR_JOG] = 0; U.step = NJOG_STEPS - 1;   /* continuous */
		double p0[NAXES]; memcpy(p0, C.mo.pos, sizeof p0);
		pendant_t q = { .axis = -1, .mult = 1 };
		ui_pendant(&q, -8);                              /* not held: nothing moves */
		q.held = true; ui_pendant(&q, 0);
		CHECK(ui_pane() == 1);                           /* held, switch OFF: the setup pane */
		ui_pendant(&q, -8); ui_pendant(&q, 8);           /* changes setup fields, never jogs */
		U.pane[SCR_JOG] = 0;
		ui_pendant(&q, -8); ui_pendant(&q, 8);           /* even on the jog pane: nothing */
		fx_run(300);
		CHECK(!C.mo.jog_dir && motion_idle(&C.mo));
		for (int a = 0; a < NAXES; a++) CHECK_NEAR(C.mo.pos[a], p0[a], 1e-9);
		q.axis = AX_Y; ui_pendant(&q, 0);                /* Y picked: jog pane, diamond */
		CHECK(ui_pane() == 0);
		C.pend = q; snap("12a_jog_pendant_axis");
		{                                                /* Run: the same tint; a JOB part G55 gets its own small axes */
			wcs_t keep = WD.wcs[1];
			memcpy(WD.wcs[1].off, WD.wcs[0].off, sizeof WD.wcs[1].off); WD.wcs[1].off[AX_X] += 90; WD.wcs[1].off[AX_Y] += 10;
			bool j0 = WD.wcs[0].job; WD.wcs[0].job = WD.wcs[1].job = true;
			U.screen = SCR_RUN; U.view.iso = false; U.view.saver = false; snap("12b_run_pendant_axis_job");
			U.view.iso = true; snap("12c_run_iso_job");
			/* every JOB part drawn; a resume point on G55 colours G54's whole section as done */
			C.resume_line = 6; C.job_idx = 1; U.pane[SCR_RUN] = 0;
			snap("12d_job_parts_pick");
			ui_key(K_TAB); ui_key(K_MOD); CHECK(C.job_idx == 0); ui_key(K_MOD); CHECK(C.job_idx == 1); ui_key(K_TAB);
			C.resume_line = -1; C.job_idx = -1;
			WD.wcs[1] = keep; WD.wcs[0].job = j0; U.view.iso = false; U.screen = SCR_JOG;
		}
		C.pend = (pendant_t){ .axis = -1 };
		q.held = false; q.axis = -1; ui_pendant(&q, 0);
		/* a continuous jog stops on any other key, and touch-off waits for a standstill */
		ui_key(K_YP); fx_run(100);
		CHECK(C.mo.jog_dir == 1);
		ui_key(K_SHIFT); ui_key(K_XM);
		CHECK(!C.mo.jog_dir);
		fx_run(20);
		CHECK(can(C_TOUCHOFF, &(arg_t){ .axis = AX_X, .dir = -1 }, SRC_PANEL) != NULL || motion_idle(&C.mo));
		fx_until_idle(5000);
		U.shift = false; U.step = 2; U.screen = SCR_RUN;
	}

	/* clockwise jogs the selected axis positive (it was reversed, as in the vendor app) */
	double xj = C.mo.pos[AX_X];
	pd.held = true; pd.axis = AX_X; pd.mult = 100;
	ui_pendant(&pd, -40);                            /* 10 detents clockwise, x100 = +1.0 mm */
	fx_until_idle(5000);
	CHECK_NEAR(C.mo.pos[AX_X] - xj, 1.0, 1e-6);
	pd.held = false; pd.axis = -1;
	ui_pendant(&pd, 0);

	/* drives off: Reset turns them on from any screen, and the banner says why */
	U.screen = SCR_FILES;
	act(C_DRIVES_OFF, NULL, SRC_API);
	CHECK(C.m == M_OFF && strstr(C.stop_cause, "API"));
	snap("12_drives_off");
	ui_key(K_F3);
	CHECK(C.m == M_IDLE);
	U.screen = SCR_RUN;

	/* E-stop + limit banners */
	SIM.ext_estop = true; fx_run(30); snap("11_estop"); SIM.ext_estop = false; fx_run(30);
	act(C_DRIVES_ON, NULL, SRC_PANEL);
	CHECK(C.m == M_IDLE);

	/* tool change by hand: the banner, Mode -> Jog allowed, F1 asks, Enter goes */
	{
		const char *tp = "G21 G90 G54\nG0 X5 Y5\nG1 Z-1 F600\nG0 Z3\nT4 M6\nG0 X20\nM30\n";
		arg_t ld = { .path = "two-tools.nc", .text = tp, .len = strlen(tp) };
		act(C_LOAD, &ld, SRC_PANEL);
		WD.loaded_tool = 1; WD.tool[3].dia = 6;
		U.screen = SCR_RUN;
		ui_key(K_F1);
		for (int i = 0; i < 6000 && C.m == M_RUN; i++) fx_run(10);
		CHECK(C.m == M_TOOL);
		snap("13_tool_change");
		ui_key(K_MODE); CHECK(U.screen == SCR_JOG);
		ui_key(K_MODE);
		ui_key(K_F1); CHECK(C.tc_confirm && C.m == M_TOOL);
		snap("13b_tool_back_prompt");
		ui_key(K_ENTER); CHECK(C.m == M_RUN && WD.loaded_tool == 4);
		fx_until_idle(30000);
		CHECK(C.m == M_IDLE);
	}
	free(fb.px);
}
