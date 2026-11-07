/* Stop, then Start, in programs written in G91 (test manager 2026-11-07, also on the oldest
 * build): the resume rebuilt the program state from where the STOP left the axes, so every
 * incremental move after it was off by the run-on of the interrupted move (0.5-0.9 mm). The
 * programs here are net zero: whatever the stop hits, the machine must end where it started. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "t.h"
#include "core/machine.h"
#include "sim/fpga_sim.h"

static char lines20[2048];

static void load_text(const char *name, const char *src)
{
	arg_t a = { .path = name, .text = src, .len = strlen(src) };
	CHECK(act(C_LOAD, &a, SRC_PANEL) && C.prog.loaded);
}

/* run, stop after ms, Start again, to the end: the error to the start position (mm) */
static double stop_and_resume(const char *src, int ms)
{
	fx_reset();
	load_text("g91.nc", src);
	double p0[NAXES];
	memcpy(p0, C.mo.pos, sizeof p0);
	if (!act(C_START, NULL, SRC_PANEL)) return 1e9;
	fx_run(ms);
	act(C_STOP, NULL, SRC_PANEL);
	fx_until_idle(20000);
	if (C.resume_line <= 0) { printf("  no resume line after the stop\n"); return 1e9; }
	if (!act(C_START, NULL, SRC_PANEL)) return 1e9;
	fx_until_idle(300000);
	double e = 0;
	for (int a = 0; a < 3; a++) e = fmax(e, fabs(C.mo.pos[a] - p0[a]));
	return e;
}

void test_resume(void)
{
	SUITE("stop + Start in G91 programs: back on the programmed path");
	size_t k = (size_t)snprintf(lines20, sizeof lines20, "G21 G91 G94 F300\n");
	for (int i = 0; i < 20; i++) k += (size_t)snprintf(lines20 + k, sizeof lines20 - k, "G1 X1\nG1 X-1\n");
	snprintf(lines20 + k, sizeof lines20 - k, "G90\nM30\n");
	static const char *arcs = "G21 G91 G17 F300\nG2 X10 Y0 I5 J0\nG2 X-10 Y0 I-5 J0\nG2 X10 Y0 I5 J0\nG2 X-10 Y0 I-5 J0\nG90\nM30\n";
	static const char *sub = "G21 G90\nM98 P100 L6\nM30\nO100\nG91 G1 X2 F300\nY2\nX-2\nY-2\nG90\nM99\n";
	static const int stops[] = { 1500, 2700, 4100 };
	double worst[3] = { 0, 0, 0 };
	for (size_t s = 0; s < ARRAY_LEN(stops); s++) {
		worst[0] = fmax(worst[0], stop_and_resume(lines20, stops[s]));
		worst[1] = fmax(worst[1], stop_and_resume(arcs, stops[s]));
		worst[2] = fmax(worst[2], stop_and_resume(sub, stops[s]));
	}
	printf("  end vs start after stop + Start: lines %.6f, arcs %.6f, M98 square %.6f mm\n", worst[0], worst[1], worst[2]);
	CHECK(worst[0] < 1e-6); CHECK(worst[1] < 1e-6); CHECK(worst[2] < 1e-6);

	/* resume-from a line that was never run: the program's start is the one the load assumes
	 * (work zero, safe Z - what the preview shows), not wherever the machine stands */
	fx_reset();
	load_text("g91.nc", lines20);
	C.resume_line = 7; C.work_done = 6;
	double w0[NAXES];
	for (int a = 0; a < NAXES; a++) w0[a] = WD.wcs[WD.active].off[a];
	w0[AX_Z] += SET.safe_z;
	CHECK(act(C_START, NULL, SRC_PANEL));
	fx_until_idle(300000);
	CHECK_NEAR(C.mo.pos[AX_X], w0[AX_X], 1e-6); CHECK_NEAR(C.mo.pos[AX_Y], w0[AX_Y], 1e-6); CHECK_NEAR(C.mo.pos[AX_Z], w0[AX_Z], 1e-6);

	/* a multi-part job with a G91 program: every part starts where the first did, relative to
	 * its own zero (before: from where the previous part ended) */
	fx_reset();
	for (int i = 0; i < NWCS; i++) WD.wcs[i].job = false;
	memcpy(WD.wcs[1].off, WD.wcs[0].off, sizeof WD.wcs[1].off);
	WD.wcs[1].off[AX_X] += 30;
	WD.wcs[0].job = WD.wcs[1].job = true; WD.active = 0;
	load_text("g91job.nc", "G21 G91 F600\nG1 X5\nG1 Y5\nG1 X-5\nG1 Y-5\nG1 X2\nG90\nM30\n");
	{
		double p0[NAXES];
		memcpy(p0, C.mo.pos, sizeof p0);
		CHECK(act(C_START, NULL, SRC_PANEL));
		fx_until_idle(300000);
		/* part 2 (G55, 30 mm further in X) ended 2 mm right of where part 1 started, shifted by 30 */
		CHECK_NEAR(C.mo.pos[AX_X], p0[AX_X] + 30 + 2, 1e-6);
		CHECK_NEAR(C.mo.pos[AX_Y], p0[AX_Y], 1e-6);
		CHECK_NEAR(C.mo.pos[AX_Z], p0[AX_Z], 1e-6);
	}
	WD.wcs[0].job = WD.wcs[1].job = false;

	/* G90 programs keep working as before: absolute targets don't depend on the start */
	double e90 = stop_and_resume("G21 G90 G94 F300\nG1 X10\nG1 X0\nG1 X10\nG1 X0\nG1 X10\nG1 X0\nM30\n", 2500);
	CHECK(e90 < 1e-6 || fabs(C.mo.pos[AX_X] - WD.wcs[WD.active].off[AX_X]) < 1e-6);
}
