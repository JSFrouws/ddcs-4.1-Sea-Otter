/* The Fanuc-style dialect: offsets (G92 G52 G43 G10), G28/G29/G30, feed modes, exact stop,
 * more cycles, M98/M99/M97 subprograms, macro B (#variables, expressions, IF/GOTO/WHILE,
 * G65), what is accepted with a warning and what refuses the load. */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "t.h"
#include "core/gcode.h"

static gc_t g;
static gc_op_t o[256];
static const double zero[NAXES];

static int run(const char *src)
{
	gc_begin(&g, src, strlen(src), zero);
	int n = 0, r;
	gc_op_t op;
	while ((r = gc_next(&g, &op)) > 0) if (n < 256) o[n++] = op;
	return r < 0 ? -1 : n;
}

/* lines only, end points X/Y/Z of each */
static int lines(const char *src, double (*p)[3], int max)
{
	int n = run(src), k = 0;
	if (n < 0) return -1;
	for (int i = 0; i < n; i++) if (o[i].kind == OP_LINE && k < max) { for (int a = 0; a < 3; a++) p[k][a] = o[i].end[a]; k++; }
	return k;
}

static bool warned(const char *s) { return strstr(g.warn, s) != NULL; }
static bool failed(const char *src, const char *msg) { return run(src) < 0 && strstr(g.err, msg) != NULL; }

void test_fanuc(void)
{
	SUITE("gcode: Fanuc dialect");
	settings_defaults(&SET);
	workdata_defaults(&WD);
	for (int i = 0; i < NWCS; i++) memset(WD.wcs[i].off, 0, sizeof WD.wcs[i].off);
	WD.active = 0;
	double p[64][3];
	int n;

	/* --- parsing: %, O, N, comments, lower case, leading zeros, block delete --- */
	n = lines("%\nO1234 (MAIN)\nN10 g00 x1. y2.\nN20 G01 X3 F100 ; cut\n(only a comment)\n%\nG0 X99\n", p, 64);
	CHECK(n == 2 && p[0][0] == 1 && p[0][1] == 2 && p[1][0] == 3 && g.prog_no == 1234);   /* nothing after the closing % */
	CHECK(!g.warn[0]);
	n = lines("G0 X1\n/G0 X2\nG0 X3\n", p, 64);
	CHECK(n == 3);                                            /* switch off: the / block runs */
	SET.gc_block_delete = true;
	n = lines("G0 X1\n/G0 X2\nG0 X3\n", p, 64);
	CHECK(n == 2 && p[1][0] == 3);
	SET.gc_block_delete = false;
	CHECK(failed("G0 X1 Q\n", "Q without a number"));
	/* numbers: the same double as strtod gives (the parser does them by hand for speed) */
	n = lines("G0 X0.1 Y123.4567 Z-.5\nG0 X0.0000000000000001 Y12345678901234567890 Z007.250\n", p, 64);
	CHECK(n == 2 && p[0][0] == 0.1 && p[0][1] == 123.4567 && p[0][2] == -0.5);
	CHECK(p[1][0] == 1e-16 && p[1][1] == 12345678901234567890.0 && p[1][2] == 7.25);

	/* --- G4: X seconds; P seconds or ms (setting); no motion from G4 X --- */
	n = run("G04 X1.5\n");
	CHECK(n == 1 && o[0].kind == OP_DWELL && o[0].value == 1.5);
	n = run("G4 P2\n");
	CHECK(n == 1 && o[0].value == 2);
	n = run("G4 P1500\n");
	CHECK(n == 1 && o[0].value == 1500 && warned("Fanuc P is ms"));
	SET.gc_dwell_ms = 1;
	n = run("G4 P1500\nG0 Z5\nG82 X0 Z-1 R1 P250 F100\n");
	CHECK(o[0].value == 1.5);
	CHECK(n == 7 && o[5].kind == OP_DWELL && fabs(o[5].value - 0.25) < 1e-12);
	SET.gc_dwell_ms = 0;

	/* --- warnings: accepted codes, every distinct one listed --- */
	n = run("G17 G21 G40 G49 G80 G64 G15 G50 G69 G97 G67\nG5.1 Q1\nG8 P1\nM19\nM48\nG0 X1\n");
	CHECK(n == 1);
	CHECK(warned("G5.1 ignored") && warned("G8 ignored") && warned("M19 ignored") && warned("M48 ignored"));
	CHECK(failed("G0 X1\nG12.1\n", "not supported") && g.err_line == 1);
	CHECK(failed("G54.1 P1\n", "only G54-G59"));
	CHECK(failed("G68 X0 Y0 R30\n", "rotation"));
	CHECK(failed("G16\n", "polar"));
	CHECK(failed("G96 S200\n", "G96"));
	CHECK(failed("G31 X10\n", "probing"));
	CHECK(failed("G84 X0 Z-5 R1\n", "tapping"));
	CHECK(failed("G87 X0 Z-5 R1\n", "orientation"));

	/* --- G92 family, G52 --- */
	n = lines("G0 X10 Y10\nG92 X0 Y0\nG0 X5\nG92.2\nG0 X1\nG92.3\nG0 X2\nG92.1\nG0 X3\n", p, 64);
	CHECK(n == 5);
	CHECK_NEAR(p[1][0], 15, 1e-9); CHECK_NEAR(p[2][0], 1, 1e-9); CHECK_NEAR(p[3][0], 12, 1e-9); CHECK_NEAR(p[4][0], 3, 1e-9);
	CHECK(failed("G92\n", "axis word"));
	WD.wcs[1].off[0] = 100;
	n = lines("G55 G52 X20 Y5\nG0 X1 Y1\nG52 X0 Y0\nG0 X1 Y1\n", p, 64);
	CHECK(n == 2 && p[0][0] == 121 && p[0][1] == 6 && p[1][0] == 101 && p[1][1] == 1);
	WD.wcs[1].off[0] = 0;
	/* G10 L20 with G92 active: the work offset is chosen so the position reads the value */
	n = lines("G0 X10\nG92 X0\nG10 L20 P1 X3\nG0 X3\n", p, 64);
	CHECK(n == 2 && fabs(p[1][0] - 10) < 1e-9);
	/* G91 G10 L2 adds (Fanuc) */
	run("G10 L2 P2 X5\nG91 G10 L2 P2 X1.5\n");
	CHECK_NEAR(g.wcs_off[1][0], 6.5, 1e-9);
	/* G10 L1 / L10 / L12: the tool table copy, never WD while not running */
	double len2 = WD.tool[1].len;
	run("G10 L1 P2 Z40 R3\nG10 L10 P3 R55\nG10 L12 P3 R4\nG10 L11 P3 R0.1\n");
	CHECK(g.tl_len[1] == 40 && g.tl_dia[1] == 6 && g.tl_len[2] == 55 && g.tl_dia[2] == 8 && warned("G10 L11 ignored"));
	CHECK(WD.tool[1].len == len2);

	/* --- G43: ignored by default (Z touched off per tool), the table length when set --- */
	WD.tool[0].len = 50;
	n = lines("T1 M6\nG43 H1 Z10\nG0 Z5\n", p, 64);
	CHECK(n == 2 && p[0][2] == 10 && warned("G43 ignored"));
	SET.gc_tlo = 1;
	n = lines("T1 M6\nG43 H1 Z10\nG0 Z5\nG49 G0 Z5\nG44 H1 Z0\n", p, 64);
	CHECK(n == 4 && p[0][2] == 60 && p[1][2] == 55 && p[2][2] == 5 && p[3][2] == -50 && !g.warn[0]);
	n = lines("T1 M6\nG43 Z10\n", p, 64);                    /* no H: the tool of the M6 */
	CHECK(n == 1 && p[0][2] == 60);
	CHECK(failed("G43 H99 Z1\n", "no such tool"));
	SET.gc_tlo = 0;

	/* --- G41/G42: refused, or ignored on purpose --- */
	CHECK(failed("G0 X0\nG41 D1 X5\n", "cutter comp") && g.err_line == 1);
	SET.gc_cutcomp = 1;
	n = lines("G41 D1 G1 X5 F100\nG40 X6\n", p, 64);
	CHECK(n == 2 && p[0][0] == 5 && warned("G41/G42 ignored"));
	SET.gc_cutcomp = 0;

	/* --- G28 / G29 / G30: via the point, then home; bare G28: Z first --- */
	for (int a = 0; a < NAXES; a++) SET.ax[a].home_pos = a == AX_Z ? 0 : -1;
	SET.ax[AX_Z].home_pos = 0;
	n = lines("G0 X10 Y20 Z-5\nG91 G28 Z0\n", p, 64);
	CHECK(n == 3 && p[1][2] == -5 && p[2][2] == 0 && p[2][0] == 10 && p[2][1] == 20);
	n = lines("G0 X10 Y20 Z-5\nG90 G28 X30\nG29 X40 Y7\n", p, 64);
	CHECK(n == 5);
	CHECK(p[1][0] == 30 && p[1][2] == -5);                    /* the intermediate point (work X30) */
	CHECK(p[2][0] == -1 && p[2][1] == 20);                    /* then X home only */
	CHECK(p[3][0] == 30 && p[4][0] == 40 && p[4][1] == 7);    /* G29: via X30, to X40 Y7 */
	n = lines("G0 X10 Y20 Z-5\nG28\n", p, 64);
	CHECK(n == 3 && p[1][2] == 0 && p[1][0] == 10 && p[2][0] == -1 && p[2][1] == -1);
	n = lines("G0 Z-5\nG91 G30 Z0\n", p, 64);
	CHECK(n == 3 && p[2][2] == 0 && warned("G30"));
	CHECK(failed("G29 X1\n", "G29 without"));

	/* --- G90.1 absolute arc centre --- */
	n = run("G0 X10 Y0\nG90.1 G3 X0 Y10 I0 J0 F500\n");
	{
		bool on = true;
		for (int i = 1; i < n; i++) on &= fabs(hypot(o[i].end[0], o[i].end[1]) - 10) < 1e-9;
		CHECK(n > 5 && on && fabs(o[n - 1].end[1] - 10) < 1e-9);
	}
	CHECK(failed("G90.1 G2 X1 Y1 I0\n", "both centre words"));

	/* --- feed modes --- */
	n = run("G93 G1 X10 F6\nG1 X10 Y10 F60\n");                /* 1/6 min for 10 mm = 60 mm/min */
	CHECK(n == 2 && fabs(o[0].feed - 60) < 1e-9 && fabs(o[1].feed - 600) < 1e-9);
	CHECK(failed("G93 G1 X10 F6\nG1 X20\n", "inverse time"));
	n = run("S2000 M3\nG95 G1 X10 F0.05\nG94 G1 X20 F300\n");
	CHECK(n == 3 && fabs(o[1].feed - 100) < 1e-9 && fabs(o[2].feed - 300) < 1e-9);
	CHECK(failed("G95 G1 X10 F0.1\n", "needs a spindle speed"));
	n = run("G93 G2 X20 Y0 I10 J0 F2\n");                      /* half circle r 10: 31.4 mm in 0.5 min */
	CHECK(n > 3 && fabs(o[0].feed - 10 * M_PI * 2) < 1e-6 && o[n - 1].feed == o[0].feed);

	/* --- G9 / G61: a stop (zero dwell) after every move; G64 back to blending --- */
	n = run("G1 X1 F100\nG9 G1 X2\nG61\nX3\nX4\nG64\nX5\n");
	CHECK(n == 8);
	CHECK(o[0].kind == OP_LINE && o[1].kind == OP_LINE && o[2].kind == OP_DWELL && o[2].value == 0);
	CHECK(o[3].kind == OP_LINE && o[4].kind == OP_DWELL && o[5].kind == OP_LINE && o[6].kind == OP_DWELL && o[7].kind == OP_LINE);

	/* --- more cycles: G86 spindle stop + restart, G76 as G89, K repeats, K0 --- */
	n = run("S1000 M3\nG0 Z5\nG86 X1 Y1 Z-2 R1 F100\n");
	CHECK(n == 8 && o[5].kind == OP_SPINDLE && o[5].ival == 0 && o[6].kind == OP_LINE && o[6].rapid && o[6].end[2] == 5
		&& o[7].kind == OP_SPINDLE && o[7].ival == 3 && o[7].value == 1000);
	n = run("G0 Z5\nG76 X1 Z-2 R1 P0.5 Q0.1 F100\n");
	CHECK(n == 7 && o[4].kind == OP_DWELL && !o[5].rapid && o[5].end[2] == 1 && o[6].end[2] == 5 && warned("G76"));
	n = lines("G0 X0 Y0 Z5\nG91 G99 G81 X10 Z-3 R-4 K3 F100\n", p, 64);   /* 3 holes, 10 mm apart */
	CHECK(n == 1 + 3 * 4);
	CHECK(p[2][0] == 10 && p[6][0] == 20 && p[10][0] == 30 && p[11][2] == -2 && p[12][2] == 1);
	n = lines("G0 X0 Y0 Z5\nG81 X10 Z-3 R1 K0 F100\nX20\n", p, 64);
	CHECK(n == 1 + 4 && p[2][0] == 20);                       /* K0 drills nothing, keeps the data */

	/* --- M98 / M99 subprograms, repeats, nesting, M97 --- */
	const char *sub =
		"O0001\n"
		"G0 X0\n"
		"M98 P1000 L2\n"
		"G0 X9\n"
		"M30\n"
		"O1000 (SUB)\n"
		"G91 G0 X1\n"
		"M98 P2000\n"
		"G90\n"
		"M99\n"
		"O2000\n"
		"G91 G0 Y1\n"
		"M99\n";
	n = lines(sub, p, 64);
	CHECK(n == 6);
	CHECK(p[1][0] == 1 && p[2][1] == 1 && p[3][0] == 2 && p[4][1] == 2 && p[5][0] == 9 && p[5][1] == 2);
	CHECK(g.flow && !g.warn[0]);
	n = run(sub);
	CHECK(o[2].line == 11 && o[n - 1].kind == OP_END && o[n - 1].line == 4);
	n = lines("G0 X0\nM98 P31000\nM30\nO1000\nG91 X1\nG90\nM99\n", p, 64);   /* P31000 = O1000 three times */
	CHECK(n == 4 && p[3][0] == 3);
	n = lines("M97 P100 L2\nG0 X50\nM30\nN100 G91 G0 X5\nG90\nM99\n", p, 64);
	CHECK(n == 3 && p[1][0] == 10 && p[2][0] == 50);
	CHECK(failed("M98 P1234\n", "no O1234"));
	CHECK(failed("O1\nM98 P2\nM30\nO2\nG0 X1\n", "no M99"));
	CHECK(failed("O100\nG0 X1\nM99\nO1\nM98 P100\nM30\n", "main program first"));
	n = run("G0 X1\nM99\n");
	CHECK(n == 1 && warned("M99 in the main program"));
	n = lines("O1\nG0 X1\nO2\nG0 X2\n", p, 64);
	CHECK(n == 1 && warned("main program ends at O2"));
	CHECK(failed("M98 P1\nM30\nO1\nM98 P1\nM99\n", "nested deeper"));

	/* seek: lines in order of execution (resume restores the state at the FIRST run of a line) */
	gc_begin(&g, sub, strlen(sub), zero);
	CHECK(gc_seek(&g, 3) == 0 && g.pos[0] == 2 && g.pos[1] == 2);      /* the main line after the calls */
	gc_op_t op;
	CHECK(gc_next(&g, &op) == 1 && op.line == 3 && op.end[0] == 9);
	gc_begin(&g, sub, strlen(sub), zero);
	CHECK(gc_seek(&g, 11) == 0 && g.pos[0] == 1 && g.pos[1] == 0 && g.sp == 2);   /* inside O2000, first call */
	CHECK(gc_next(&g, &op) == 1 && op.line == 11 && op.end[1] == 1);

	/* --- macro B --- */
	n = lines("#1 = 2\n#2 = [#1 + 3] * 2\n#100 = SQRT[16] + ABS[-1] + ROUND[1.6] + FIX[2.7] + FUP[2.1]\n"
		"G0 X#2 Y#100 Z-#1\n#3 = SIN[30] + COS[60] + ATAN[1]/[1] + POW[2, 3] + 7 MOD 4\nG0 X#3\n", p, 64);
	CHECK(n == 2 && p[0][0] == 10 && p[0][1] == 4 + 1 + 2 + 2 + 3 && p[0][2] == -2);
	CHECK_NEAR(p[1][0], 0.5 + 0.5 + 45 + 8 + 3, 1e-9);
	n = lines("G0 X5 Y5\nG0 X#7 Y[#0 + 1]\n", p, 64);         /* a vacant variable leaves the word out */
	CHECK(n == 2 && p[1][0] == 5 && p[1][1] == 1);
	n = lines("#1 = 0\nWHILE [#1 LT 5] DO1\nG0 X#1\n#1 = #1 + 1\nEND1\nG0 Y9\n", p, 64);
	CHECK(n == 6 && p[4][0] == 4 && p[5][1] == 9);
	n = lines("#1 = 1\nN10 G0 X#1\n#1 = #1 * 2\nIF [#1 LE 8] GOTO 10\nIF [#1 EQ 16] THEN #2 = 7\nG0 Y#2\n", p, 64);
	CHECK(n == 5 && p[3][0] == 8 && p[4][1] == 7);
	n = lines("#1 = 3\nWHILE [#1 GT 5] DO2\nG0 X1\nEND2\nG0 X2\n", p, 64);   /* false at once: skips the body */
	CHECK(n == 1 && p[0][0] == 2);
	CHECK(run("IF [#5 EQ #0] GOTO 20\nG0 X1\nN20 G0 X2\n") == 1 && o[0].end[0] == 2);   /* vacant EQ vacant */
	CHECK(run("#1 = 0\nIF [#1 EQ #0] GOTO 20\nG0 X1\nN20 G0 X2\n") == 2);              /* 0 is not vacant */
	CHECK(run("IF [[1 GT 0] AND [2 GT 1]] GOTO 5\nG0 X1\nN5 G0 X2\n") == 1);
	/* G65 with arguments (A=#1 B=#2 X=#24), its own locals */
	n = lines("#1 = 99\nG65 P9010 A2 B3 X10\nG0 Y#1\nM30\nO9010\nG0 X[#24 + #1 * #2]\nM99\n", p, 64);
	CHECK(n == 2 && p[0][0] == 16 && p[1][1] == 99);
	/* system variables: positions, offsets, modal state */
	WD.wcs[0].off[0] = 7;
	n = lines("G90 G0 X3\n#1 = #5021\n#2 = #5041\n#3 = #5221\n#4 = #4003\nG91\n#5 = #4003\nG90 G0 Y#1 Z#2 X[#3 + #4 + #5]\n", p, 64);
	CHECK(n == 2 && p[1][1] == 10 && p[1][2] == 3 && p[1][0] == 7 + 7 + 90 + 91);
	run("#5221 = 12\n");
	CHECK(g.wcs_off[0][0] == 12 && WD.wcs[0].off[0] == 7);   /* the copy, not WD (preview) */
	WD.wcs[0].off[0] = 0;
	CHECK(failed("(PART TOO BIG)\n#3000 = 1 (PART TOO BIG)\n", "PART TOO BIG"));
	n = run("G0 X1\n#3006 = 1 (CHECK CLAMPS)\nG0 X2\n");
	CHECK(n == 3 && o[1].kind == OP_PAUSE && o[1].ival == PAUSE_M0 && warned("CHECK CLAMPS"));
	CHECK(failed("#1 = 1 / 0\n", "division by zero"));
	CHECK(failed("#1 = [2 + 3\n", "']' expected"));
	CHECK(failed("#1 = #1000\n", "#1000 not supported"));
	CHECK(failed("WHILE [1 EQ 1] DO1\nEND1\n", "endless loop"));
	CHECK(failed("END1\n", "END1 without"));
	CHECK(failed("WHILE [0] DO1\nG0 X1\n", "without END1"));
	CHECK(failed("GOTO 77\n", "no N77"));
	n = lines("#500 = 4\nG0 X#500\n", p, 64);
	CHECK(n == 1 && p[0][0] == 4);
	n = lines("G0 X#501\n", p, 64);
	CHECK(n == 0 && warned("#500-#999"));

	/* inch units in macro positions and words */
	n = lines("G20 G0 X1\n#1 = #5041\nG0 Y#1\n", p, 64);
	CHECK(n == 2 && fabs(p[1][1] - 25.4) < 1e-9);

	settings_defaults(&SET);
	workdata_defaults(&WD);
}
