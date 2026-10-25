/* Program run time: what the motion planner will do, per line, at two feed overrides.
 *
 * The model is the planner's own (core/motion.c): each line segment gets the planner's
 * geometry (speed caps per axis incl. the FPGA's sustained 32 000 steps/s, acceleration,
 * jerk -> the S-curve's average acceleration) and corner limit; a backward + forward pass
 * gives every segment's entry speed, and a trapezoid gives its time. Every non-motion op
 * (M3/M5, M7-M9, M0/M1/M6, dwell) stops the motion, as it does in the planner. Dwells and
 * the spindle spin-up wait (spindle.delay, at every M3/M4) are added as they are.
 * A tool change by hand counts nothing: the wait for the operator is not known.
 *
 * The times are kept per line, cumulative, at overrides 100 % and 200 %. Below 100 % they
 * are scaled by 1/override (an over-estimate: acceleration phases scale less); between 100
 * and 200 % interpolated linearly in 1/override (also over: time is convex in 1/override);
 * above 200 % the 200 % time is used (over). Rapids follow the rapid override, min(feed, 100).
 * Then a safety margin is added (EST_MARGIN, EST_MARGIN_S): the user prefers too long to
 * too short.
 *
 * It is computed in the BACKGROUND after a program loads (est_background, a few ms per
 * controller tick): on the unit (no FPU) it takes several seconds for a 64 000-line program,
 * which would otherwise make loading several times slower. Until it is done the time shows "…". */
#ifndef ESTIMATE_H
#define ESTIMATE_H
#include "program.h"
#include "gcode.h"

#define EST_NLV 2                   /* override levels: 100, 200 % */
/* Measured against the real planner + executor on the simulated machine (tests/test_estimate.c):
 * the model alone comes out 1-4 % short (each start / stop loses a few ms to the 10 ms slices
 * and the S-curve's ends; worst: drilling cycles, 3.6 %). 5 % + 1 s covers that. */
#define EST_MARGIN 1.05             /* x total ... */
#define EST_MARGIN_S 1.0            /* ... + this, in seconds */

/* work on the loaded program's estimate for about budget_ms (call every tick); true = done */
bool est_background(int budget_ms);
/* compute a program's estimate now, all of it (tests) */
void est_compute(program_t *p);

/* seconds from line `from` (0-based, included) to the end, at feed override ov % (margin in) */
double est_remaining(const program_t *p, int from, int ov);
/* seconds of lines [a, b) at override ov % (no margin); -1 = not known */
double est_span(const program_t *p, int a, int b, int ov);
/* What the Run screen shows, at the current feed override and for every JOB part of a
 * multi-part job: while running / on hold / at a tool change the time left (*left = true),
 * otherwise the whole run. -1 = not known (no program, feed override 0 %), -2 = still being
 * worked out. */
double est_now(bool *left);

#endif
