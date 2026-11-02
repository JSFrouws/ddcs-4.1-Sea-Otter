/* Work offsets vs. the program (user 2026-11-02): a G54-G59 the program writes itself is
 * fixed in the job, Run's WCS menu only opens what is in the job or the code, and the
 * preview is placed per vertex by the WCS its move went through, with the offsets as they
 * are now. */
#include <string.h>
#include "t.h"
#include "ui/ui.h"

static void load(const char *name, const char *text)
{
	arg_t a = { .path = name, .text = text, .len = strlen(text) };
	CHECK(act(C_LOAD, &a, SRC_PANEL));
	CHECK(C.prog.loaded);
}

static const pv_t *vertex_of_line(int line)
{
	for (int i = 0; i < C.prog.npv; i++) if (C.prog.pv[i].line == line) return &C.prog.pv[i];
	return NULL;
}

void test_wcs(void)
{
	SUITE("wcs: G-code references fix the job, the menu, the preview placement");
	/* no program: every WCS selectable (the default job mark on G54 alone does not close the menu for the others?
	 * it does - the set is {G54}; with no marks at all it is empty and everything opens) */
	fx_reset();
	for (int i = 0; i < NWCS; i++) WD.wcs[i].job = false;
	for (int i = 0; i < NWCS; i++) CHECK(ctl_wcs_selectable(i) && !ctl_wcs_in_job(i) && !ctl_wcs_fixed(i));
	WD.wcs[2].job = true;
	CHECK(ctl_wcs_selectable(2) && !ctl_wcs_selectable(0) && !ctl_wcs_selectable(5));

	/* one WCS in the code (the CAM case: G59 in the header): fixed in the job, the operator may add parts */
	load("one.nc", "G21 G90 G59\nG0 Z5\nG0 X0 Y0\nG1 Z-1 F300\nG1 X20\nG0 Z5\nM30\n");
	CHECK(C.prog.code_wcs == 1 << 5 && !C.prog.multi_wcs && C.prog.pv_wcs0 == 5);
	CHECK(ctl_wcs_fixed(5) && ctl_wcs_in_job(5) && !WD.wcs[5].job);
	CHECK(ctl_job_parts() == 2);                       /* G56 (marked) + G59 (code) */
	CHECK(ctl_wcs_selectable(2) && ctl_wcs_selectable(5) && !ctl_wcs_selectable(0));
	U.screen = SCR_WORK; U.pane[SCR_WORK] = 0; U.wcs_row = 5;
	ui_key(K_F4);                                      /* refused: it stays in the job */
	CHECK(!WD.wcs[5].job && ctl_wcs_in_job(5));
	U.wcs_row = 0; ui_key(K_F4); CHECK(WD.wcs[0].job && ctl_job_parts() == 3);   /* a part can still be added */
	ui_key(K_SHIFT); ui_key(K_F4);                     /* clear the job: the marks go, the code's WCS stays */
	CHECK(!WD.wcs[0].job && !WD.wcs[2].job && ctl_wcs_in_job(5) && ctl_job_parts() == 1);
	for (int i = 0; i < NWCS; i++) CHECK(ctl_wcs_selectable(i) == (i == 5));
	/* Run's menu refuses the others, takes G59 */
	WD.active = 0; U.screen = SCR_RUN; U.pane[SCR_RUN] = 0;
	ui_key(K_SEL); CHECK(U.wcssel);
	ui_key(K_F1); CHECK(WD.active == 0 && !U.wcssel);  /* G54: not in the job or the code */
	ui_key(K_SEL); ui_key(K_F6); CHECK(WD.active == 5);

	/* two in the code: both fixed, no multi-part job (it runs once), nothing can be added */
	load("two.nc", "G21 G90 G54\nG0 Z5\nG1 Z-1 F300\nG1 X20\nG0 Z5\nG55\nG0 X0 Y0\nG1 Z-1\nG1 Y20\nG0 Z5\nM30\n");
	CHECK(C.prog.code_wcs == 3 && C.prog.multi_wcs);
	CHECK(ctl_wcs_fixed(0) && ctl_wcs_fixed(1) && ctl_job_parts() == 0 && ctl_resume_part() < 0);
	CHECK(ctl_wcs_selectable(0) && ctl_wcs_selectable(1) && !ctl_wcs_selectable(2));
	U.screen = SCR_WORK; U.wcs_row = 2; ui_key(K_F4); CHECK(!WD.wcs[2].job);
	program_free(&C.prog);
	for (int i = 0; i < NWCS; i++) CHECK(!ctl_wcs_fixed(i));

	/* the preview: the user's square (G91 moves), loaded with G54 active and G54 at (366.5,
	 * 226.58, -43.195), then G59 (366.5, 236.8, -48.195) made active: every vertex went
	 * through G54, so seen from G59 the square sits 10.22 mm lower and 5 mm higher in Z -
	 * exactly where the machine will cut it, not far away */
	fx_reset();
	for (int i = 0; i < NWCS; i++) WD.wcs[i].job = false;
	const double g54[3] = { 366.5007, 226.58, -43.195 }, g59[3] = { 366.5007, 236.8, -48.195 };
	for (int a = 0; a < 3; a++) { WD.wcs[0].off[a] = g54[a]; WD.wcs[5].off[a] = g59[a]; }
	WD.active = 0;
	load("square.nc", "G21\nG91\nG0 X10\nG0 X-10\nG1 X5 Y5 F300\nG1 X-5 Y-5\nG90\nM30\n");
	CHECK(C.prog.code_wcs == 0 && C.prog.pv_wcs0 == 0);
	const pv_t *v = vertex_of_line(4);                 /* G1 X5 Y5: work (5, 5) of G54 */
	CHECK(v && v->wcs == 0);
	double o[3];
	ui_view_place(v, -1, o);
	CHECK_NEAR(o[0], 5, 1e-3); CHECK_NEAR(o[1], 5, 1e-3);
	WD.active = 5;
	ui_view_place(v, -1, o);
	CHECK_NEAR(o[0], 5, 1e-3); CHECK_NEAR(o[1], 5 - 10.22, 1e-3); CHECK_NEAR(o[2], v->z + 5, 1e-3);
	/* a touch-off of G54 after the load moves the picture with it */
	WD.active = 0; WD.wcs[0].off[0] += 3;
	ui_view_place(v, -1, o);
	CHECK_NEAR(o[0], 5, 1e-3);                         /* seen from G54 itself: still at 5 */
	WD.active = 5;
	ui_view_place(v, -1, o);
	CHECK_NEAR(o[0], 8, 1e-3);                         /* from G59: 3 mm further */
	/* a part of a job: the vertex goes through that part's offset instead */
	ui_view_place(v, 5, o);
	CHECK_NEAR(o[0], 5, 1e-3); CHECK_NEAR(o[1], 5, 1e-3);
	/* the same square, absolute, with G59 in its header, loaded while G54 is active: it is
	 * G59's, so with G59 active it sits at the origin, with G54 active it shows at G59's zero
	 * (an incremental program instead starts from where the machine is: G54's zero) */
	WD.wcs[0].off[0] -= 3; WD.active = 0;
	load("square59.nc", "G21 G90 G59\nG0 X10\nG0 X-10\nG1 X5 Y5 F300\nG1 X-5 Y-5\nM30\n");
	CHECK(C.prog.pv_wcs0 == 5 && ctl_wcs_fixed(5));
	v = vertex_of_line(3);
	CHECK(v && v->wcs == 5);
	ui_view_place(v, -1, o);
	CHECK_NEAR(o[1], 5 + 10.22, 1e-3);                 /* seen from G54 */
	WD.active = 5;
	ui_view_place(v, -1, o);
	CHECK_NEAR(o[0], 5, 1e-3); CHECK_NEAR(o[1], 5, 1e-3);
	U.view.iso = false; U.screen = SCR_RUN;
	program_free(&C.prog);
	fx_reset();
}
