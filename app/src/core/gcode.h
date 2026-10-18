/* Streaming G-code interpreter: text in, machine-coordinate operations out.
 *
 * Supported: G0 G1 G2 G3 (IJK or R, planes G17/18/19), G4, G10 L2/L20, G17-19, G20/21,
 * G28 (axes to their home position), G53, G54-G59, G90/91, G94,
 * canned cycles G73 G81 G82 G83 G85 G89 with G98/G99, R Z Q P, modal repeats, cancelled by
 * G80 or G0-G3 (P dwell in seconds, as G4),
 * M0 M1 M2 M3 M4 M5 M6 M7 M8 M9 M10 M11 M30, F S T N, comments ( ) and ;.
 * Ignored with a warning: G40 G43 G49 G61 G64 (no cutter/length comp in this machine).
 * Arcs are cut into chords (0.005 mm sagitta) one at a time, so memory stays flat. */
#ifndef GCODE_H
#define GCODE_H
#include "settings.h"

typedef enum { OP_LINE, OP_DWELL, OP_SPINDLE, OP_COOLANT, OP_PAUSE, OP_TOOL, OP_END } op_kind_t;
enum { PAUSE_M0, PAUSE_M1, PAUSE_TOOL };

typedef struct {
	op_kind_t kind;
	bool rapid;
	double end[NAXES];   /* machine coordinates, mm */
	double feed;         /* mm/min */
	int line;            /* 0-based source line */
	int arc;             /* != 0: chord of arc #arc (lets the planner keep speed) */
	double value;        /* S rpm, dwell s, tool number */
	int ival;            /* spindle 0/3/4, coolant mask (1 flood, 2 mist, 4 aux), pause kind */
} gc_op_t;

typedef struct {
	/* modal state - also what "resume from line" restores */
	bool absolute, inches, retract_r;
	int plane, wcs, motion, spindle, coolant, tool;
	int cycle;                    /* 0, or the active canned cycle (73, 81 ...) */
	double cy_r, cy_z, cy_q, cy_p; /* its modal R / bottom Z (machine mm), peck Q (mm), dwell P (s) */
	double cy_init;               /* G98 return level: Z when the cycle started */
	double feed, rpm;
	double pos[NAXES];
	/* source */
	const char *text; size_t len, cur;
	int line;
	/* pending output (one block can yield several ops) */
	gc_op_t q[16]; int qn, qi;
	/* arc in progress */
	struct { bool on; int n, i, a0, a1, a2; double c0, c1, r, start, sweep, h0, dh; double end[NAXES]; double feed; int line; } arc;
	int arc_seq;
	/* hole in progress (pecks are emitted a few ops at a time, like arc chords) */
	struct { bool on; int phase, n, line; double depth, ret; double p[NAXES]; } hole;
	double default_feed;
	double wcs_off[NWCS][NAXES];  /* private copy: G10 in a preview/seek must not touch WD */
	bool apply_g10;               /* real run: G10 also updates WD */
	int force_wcs;                /* >= 0: multi-part job, every G54-G59 maps to this WCS */
	char err[96]; int err_line;
	char warn[96]; int warn_line;
} gc_t;

void gc_begin(gc_t *g, const char *text, size_t len, const double machine_pos[NAXES]);
/* 1 = op written, 0 = end of program, -1 = error (g->err, g->err_line) */
int gc_next(gc_t *g, gc_op_t *op);
/* Interpret up to (not including) line `line` without emitting motion: rebuilds the
 * modal state and position for a resume. Returns 0, or -1 on error. */
int gc_seek(gc_t *g, int line);

#endif
