/* G-code interpreter: modal state, coordinate systems, arcs, errors, seek. */
#include <string.h>
#include "t.h"
#include "core/gcode.h"

static int run(const char *src, gc_op_t *ops, int max, gc_t *g, const double *start)
{
	static const double zero[NAXES];
	gc_begin(g, src, strlen(src), start ? start : zero);
	int n = 0, r;
	gc_op_t op;
	while ((r = gc_next(g, &op)) > 0) if (n < max) ops[n++] = op;
	return r < 0 ? -1 : n;
}

void test_gcode(void)
{
	SUITE("gcode interpreter");
	settings_defaults(&SET);
	workdata_defaults(&WD);
	gc_t g;
	gc_op_t o[64];

	/* G54 offset applied, G53 bypasses it, G91 relative, inch units */
	memset(&WD.wcs[0], 0, sizeof WD.wcs[0]);
	WD.wcs[0].off[0] = 100; WD.wcs[0].off[1] = 50; WD.wcs[0].off[2] = -80;
	WD.active = 0;
	int n = run("G21 G90 G54\nG0 X10 Y5 Z2\nG53 G0 Z-10\nG91 G1 X1 F600\nG20 G1 X1\n", o, 64, &g, NULL);
	CHECK(n == 4);
	CHECK_NEAR(o[0].end[0], 110, 1e-9); CHECK_NEAR(o[0].end[1], 55, 1e-9); CHECK_NEAR(o[0].end[2], -78, 1e-9);
	CHECK(o[0].rapid && o[0].line == 1);
	CHECK_NEAR(o[1].end[2], -10, 1e-9);                       /* G53: machine coordinates */
	CHECK_NEAR(o[2].end[0], 111, 1e-9); CHECK(!o[2].rapid); CHECK_NEAR(o[2].feed, 600, 1e-9);
	CHECK_NEAR(o[3].end[0], 111 + 25.4, 1e-9);

	/* G55 + G10 L20: set G55 X so the current position reads 0 */
	n = run("G0 X5\nG10 L20 P2 X0\nG55 G0 X1\n", o, 64, &g, NULL);
	CHECK(n == 2);
	CHECK_NEAR(g.wcs_off[1][0], 105, 1e-9);
	CHECK_NEAR(o[1].end[0], 106, 1e-9);
	CHECK_NEAR(WD.wcs[1].off[0], 0, 1e-9);                     /* preview/seek never touch WD */

	/* full circle by IJ: ends where it started, stays on the radius */
	memset(&WD.wcs[0], 0, sizeof WD.wcs[0]);
	n = run("G0 X10 Y0\nG2 X10 Y0 I-10 J0 F1000\n", o, 64, &g, NULL);
	CHECK(n == 64);   /* capped by the array: a 10 mm circle at 0.005 mm sagitta is ~100 chords */
	n = 0;
	{
		gc_begin(&g, "G0 X10 Y0\nG2 X10 Y0 I-10 J0 F1000\n", 33, (double[NAXES]){ 0 });
		gc_op_t op; double maxerr = 0; int cnt = 0, r;
		while ((r = gc_next(&g, &op)) > 0) {
			if (!op.arc) continue;
			cnt++;
			double rr = sqrt(op.end[0] * op.end[0] + op.end[1] * op.end[1]);
			if (fabs(rr - 10) > maxerr) maxerr = fabs(rr - 10);
			if (cnt == 1) CHECK(op.end[1] < 0);               /* G2 from +X goes clockwise: down first */
		}
		CHECK(cnt > 50);
		CHECK(maxerr < 1e-9);
		CHECK_NEAR(g.pos[0], 10, 1e-9); CHECK_NEAR(g.pos[1], 0, 1e-9);
	}
	/* R-format quarter arc */
	n = run("G0 X0 Y0\nG3 X10 Y10 R10\n", o, 64, &g, NULL);
	CHECK(n > 2);
	CHECK_NEAR(o[n - 1].end[0], 10, 1e-9); CHECK_NEAR(o[n - 1].end[1], 10, 1e-9);

	/* spindle, coolant, pause, end; comments */
	n = run("(head) M3 S8000 ; spin\nM8\nM6 T4\nM0\nG4 P1.5\nM5 M9\nM30\nG0 X99\n", o, 64, &g, NULL);
	CHECK(n == 8);
	CHECK(o[0].kind == OP_SPINDLE && o[0].ival == 3 && o[0].value == 8000);
	CHECK(o[1].kind == OP_COOLANT && o[1].ival == 1);
	CHECK(o[2].kind == OP_PAUSE && o[2].ival == PAUSE_TOOL && o[2].value == 4);
	CHECK(o[3].kind == OP_PAUSE && o[3].ival == PAUSE_M0);
	CHECK(o[4].kind == OP_DWELL && o[4].value == 1.5);
	CHECK(o[5].kind == OP_SPINDLE && o[5].ival == 0);
	CHECK(o[6].kind == OP_COOLANT && o[6].ival == 0);
	CHECK(o[7].kind == OP_END);                                /* nothing after M30 */

	/* G81 drilling cycle */
	n = run("G0 Z5\nG81 X10 Y10 Z-3 R2 F200\n", o, 64, &g, NULL);
	CHECK(n == 5 && !o[3].rapid && o[3].end[2] == -3 && o[4].end[2] == 5);

	/* G83 peck drilling as Fusion writes it: G98, modal repeats on bare X/Y lines, G80, then
	 * the G0 mode from before is back */
	memset(&WD.wcs[2], 0, sizeof WD.wcs[2]);
	gc_op_t big[64];
	n = run("G56 G90 G0 X0 Y0 Z15\nG98 G83 X330 Y15 Z-10 R5 Q4 F240\nY-15\nX-330\nG80\nG0 Z20\nX1\n", big, 64, &g, NULL);
	CHECK(n == 1 + 3 * 13 + 2);
	static const double z1[] = { 15, 5, 1, 5, 1.25, -3, 5, -2.75, -7, 5, -6.75, -10, 15 };
	bool ok = true;
	for (int i = 0; i < 13; i++) {
		gc_op_t *q = &big[1 + i];
		ok &= fabs(q->end[2] - z1[i]) < 1e-9 && q->end[0] == 330 && q->end[1] == 15;
		ok &= q->rapid == !(i == 2 || i == 5 || i == 8 || i == 11);
		if (!q->rapid) ok &= q->feed == 240;
	}
	CHECK(ok);
	CHECK(big[14].end[0] == 330 && big[14].end[1] == -15 && big[14].end[2] == 15);   /* 2nd hole: Y only */
	CHECK(big[27].end[0] == -330 && big[27].end[1] == -15);
	CHECK(big[39].end[2] == 15 && big[39].line == 3);
	CHECK(big[40].rapid && big[40].end[2] == 20 && big[41].rapid && big[41].end[0] == 1);

	/* G99 returns to R; G73 backs off a little; G82 dwells; Z above R is an error */
	n = run("G56 G0 X0 Y0 Z15\nG99 G81 X1 Y1 Z-2 R3 F100\nX2\n", o, 64, &g, NULL);
	CHECK(n == 9 && o[4].end[2] == 3 && o[8].end[2] == 3 && o[8].end[0] == 2);
	n = run("G56 G0 X0 Y0 Z5\nG73 X0 Y0 Z-2 R1 Q1 F100\n", o, 64, &g, NULL);
	CHECK(n == 9 && o[3].end[2] == 0 && o[4].rapid && fabs(o[4].end[2] - 0.25) < 1e-9 && o[7].end[2] == -2 && o[8].end[2] == 5);
	n = run("G56 G0 Z5\nG82 X0 Z-1 R1 P0.5 F100\n", o, 64, &g, NULL);
	CHECK(n == 6 && o[4].kind == OP_DWELL && o[4].value == 0.5);
	CHECK(run("G56 G0 Z5\nG81 X0 Z2 R1\n", o, 64, &g, NULL) < 0 && g.err_line == 1);
	CHECK(run("G56 G0 Z5\nG84 X0 Z-2 R1\n", o, 64, &g, NULL) < 0);
	/* fine pecks: 1000 of them, emitted a few at a time */
	const char *fine = "G56 G0 Z5\nG83 X0 Z-10 R0 Q0.01 F100\n";
	gc_begin(&g, fine, strlen(fine), (double[NAXES]){ 0 });
	int nf = 0, rf;
	gc_op_t of;
	double zmin = 1e9;
	while ((rf = gc_next(&g, &of)) > 0) { nf++; zmin = fmin(zmin, of.end[2]); }
	CHECK(rf == 0 && nf == 1 + 2 + 1000 + 999 * 2 + 1 && fabs(zmin + 10) < 1e-9);
	/* resume after a cycle: Z is at the G98 level */
	const char *cyc = "G56 G0 X0 Y0 Z12\nG98 G81 X5 Z-3 R2 F100\nX6\nG80\nG0 X0\n";
	gc_begin(&g, cyc, strlen(cyc), (double[NAXES]){ 0 });
	CHECK(gc_seek(&g, 3) == 0 && g.pos[0] == 6 && g.pos[2] == 12 && g.cycle == 81 && g.cy_z == -3);

	/* errors carry their line */
	CHECK(run("G0 X1\nG41 X2\n", o, 64, &g, NULL) < 0 && g.err_line == 1);
	CHECK(run("G0 X1\nG2 X20 Y0 R5\n", o, 64, &g, NULL) < 0 && g.err_line == 1);
	CHECK(run("G0 X1 Q\n", o, 64, &g, NULL) < 0);

	/* seek rebuilds modal state for resume */
	const char *prog = "G21\nG55\nM3 S9000\nM8\nG1 X3 Y4 F444\nG0 Z9\nG1 X7\n";
	gc_begin(&g, prog, strlen(prog), (double[NAXES]){ 0 });
	CHECK(gc_seek(&g, 6) == 0);
	CHECK(g.wcs == 1 && g.spindle == 3 && g.rpm == 9000 && g.coolant == 1 && g.feed == 444);
	CHECK_NEAR(g.pos[2], 9 + WD.wcs[1].off[2], 1e-9);
	gc_op_t op;
	CHECK(gc_next(&g, &op) == 1 && op.line == 6);
}
