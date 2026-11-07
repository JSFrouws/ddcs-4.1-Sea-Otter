/* Streaming G-code interpreter: text in, machine-coordinate operations out. Fanuc-style
 * dialect (most LinuxCNC / Grbl programs read the same). The full list, with what each code
 * does here, is in docs/user-manual.md "Supported G-code"; in short:
 *
 *   motion     G0 G1 G2 G3 (IJK or R; G17/G18/G19; G90.1/G91.1), G4 (P or X), G9, G61/G64,
 *              G27 G28 G29 G30, G53, G93/G94/G95 (inverse time, per minute, per revolution)
 *   transforms G68/G69 rotation, G51/G50 scaling, G51.1/G50.1 mirror, G16/G15 polar
 *   offsets    G54-G59, G52 (local shift), G92/G92.1/G92.2/G92.3, G10 L2/L20 (work offsets),
 *              G10 L1/L10/L12 (tool table), G43/G44 Hn/G49 (Settings > G-code), G20/G21
 *   cycles     G73 G76 G81 G82 G83 G85 G86 G89 with G98/G99, R Z Q P, K/L repeats, G80
 *   program    M0 M1 M2 M30 M60, M3 M4 M5, M6, M7 M8 M9 M10 M11, M98/M99 (O subprograms in
 *              the same file, P and L repeats), M97 (local, by N), N, O, %, ( ) ;, block
 *              delete /
 *   macro B    #1-#33 (local), #100-#199, #500-#999, system variables (positions, offsets,
 *              modal state, #3000 alarm, #3006 stop), [expressions], functions, IF GOTO,
 *              IF THEN, GOTO, WHILE DO / END, G65 calls with arguments
 *
 * A code that can be ignored without cutting anything different is accepted with a warning
 * (shown when the program loads); a code that would cut wrong or be unsafe if ignored
 * refuses the load, and the message says why. Unknown G codes refuse, unknown M codes warn.
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

#define GC_DEPTH 8           /* M98 / M97 / G65 nesting */
#define GC_MAX_JUMPS 1000000 /* GOTO / loop / call jumps per run: more = an endless loop */

typedef struct {
	/* modal state - also what "resume from line" restores */
	bool absolute, inches, retract_r;
	int plane, wcs, motion, spindle, coolant, tool;
	int cycle;                    /* 0, or the active canned cycle (73, 81 ...) */
	double cy_r, cy_z, cy_q, cy_p; /* its modal R / bottom Z (machine mm), peck Q (mm), dwell P (s) */
	double cy_init;               /* G98 return level: Z when the cycle started */
	double feed, rpm;             /* feed: mm/min (G94) */
	int feedmode;                 /* 94, 93 (inverse time), 95 (per revolution) */
	double feed_rev, invf, bfeed; /* G95 mm/rev, G93 1/min, the feed of the move being built */
	bool arc_abs;                 /* G90.1: I J K are absolute */
	bool exact;                   /* G61: stop at the end of every move */
	int ccomp;                    /* 40, or 41/42 accepted and ignored (Settings > G-code) */
	int tlo_code;                 /* 43, 44, 49 */
	double tlo;                   /* active tool length offset, mm, added to Z (0 unless Settings > G-code says so) */
	double g52[NAXES], g92[NAXES], g92_saved[NAXES];   /* local shift (G52), G92 shift: on top of G54-G59 */
	double mid[NAXES]; bool mid_set;                   /* G28/G30 intermediate point (machine), for G29 */
	double pos[NAXES];
	/* source */
	const char *text; size_t len, cur;
	int line;
	int prog_no, seq_no;          /* the O of the main program, the last N */
	bool started;                 /* a block was executed: a later O or % ends the main program */
	/* pending output (one block can yield several ops) */
	gc_op_t q[16]; int qn, qi;
	/* arc in progress */
	struct { bool on, stop; int n, i, a0, a1, a2; double c0, c1, r, start, sweep, h0, dh; double end[NAXES]; double feed; int line; } arc;
	int arc_seq;
	/* hole in progress (pecks are emitted a few ops at a time, like arc chords) */
	struct { bool on; int phase, n, line, reps; double depth, ret; double p[NAXES]; double inc[2]; } hole;
	double default_feed;
	double wcs_off[NWCS][NAXES];  /* private copy: G10 in a preview/seek must not touch WD */
	double tl_len[NTOOLS], tl_dia[NTOOLS];   /* ... and of the tool table */
	bool apply_g10;               /* real run: G10 (and #5221... writes) also update WD */
	int force_wcs;                /* >= 0: multi-part job, every G54-G59 maps to this WCS */
	/* subprograms, macro calls, loops */
	struct { size_t ret, sub; int ret_line, sub_line, reps; bool macro; } st[GC_DEPTH]; int sp;
	struct { size_t cur; int line; bool on; } dos[GC_DEPTH + 1][4];   /* WHILE ... DO1-3, per call level */
	struct { char letter; int num; size_t off; int line; } lbl[16]; int nlbl;
	long jumps;
	bool flow;                    /* the program jumps (M98, M97, G65, GOTO, WHILE): lines don't run in order */
	int lvl;                      /* macro (G65) level: which local variables */
	double loc[GC_DEPTH + 1][33], com[100], keep[500];   /* #1-#33, #100-#199, #500-#999; NAN = vacant */
	bool emit_now, bad, keep_warned;
	uint8_t named;                /* bit a: a move of the program sets axis a (the load box shows only those) */
	bool shift;
	/* coordinate transforms in the work frame, applied to program positions (and arc centres)
	 * in this order: G51.1 mirror, G51 scale, G68 rotation. G16: plane words are radius/angle */
	bool xf;                      /* any of mirror / scale / rotation is on */
	bool mir[NAXES]; double mir_c[NAXES];
	double sc[NAXES], sc_c[NAXES];
	int rot_a0, rot_a1; double rot_c0, rot_c1, rot_deg;
	bool polar; double pol_r, pol_a;                   /* any G52 / G92 / tool length offset is non-zero (off() adds them) */
	char cmt[64];                 /* the block's comment: the message of #3000 / #3006 */
	char err[112]; int err_line;
	char warn[200]; int warn_line, warn_more;
} gc_t;

void gc_begin(gc_t *g, const char *text, size_t len, const double machine_pos[NAXES]);
/* 1 = op written, 0 = end of program, -1 = error (g->err, g->err_line) */
int gc_next(gc_t *g, gc_op_t *op);
/* Interpret up to (not including) line `line` without emitting motion: rebuilds the
 * modal state and position for a resume. With subprograms and loops this is the FIRST time
 * the program runs that line. Returns 0, or -1 on error. */
int gc_seek(gc_t *g, int line);
/* Long seeks call this every 1024 lines (main sets it: it streams the motion already
 * planned). A multi-part job seeks to its tool segment at every part switch - 59 000 lines,
 * 1.3 s on the controller - while the previous part's last moves are still running. */
extern void (*gc_yield)(void);

#endif
