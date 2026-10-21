/* Motion: turns operations into step bursts, one 10 ms slice at a time.
 *
 * Software interpolation with a lookahead planner: the queue holds at least SET.lookahead
 * segments AND at least the braking distance from top speed (so runs of tiny engraving or
 * arc segments still see far enough). Corner speeds come from a per-axis "corner jump"
 * (the speed change an axis may take instantly) plus a junction-deviation tolerance; a
 * backward pass makes every segment slow down in time for what follows, a forward pass
 * limits it to what acceleration can reach. Execution is an S-curve: acceleration itself
 * ramps with the per-axis jerk. Each slice, each axis gets one FPGA burst. This uses only the
 * burst command proven on hardware (the vendor's MPG-jog sequence); the FPGA's own
 * coordinated-move block (regs 0x50..0x60) isn't decoded yet. */
#ifndef MOTION_H
#define MOTION_H
#include "gcode.h"
#include "../hal/fpga.h"

#define MQ_LEN 512

typedef enum { MEV_NONE, MEV_SPINDLE, MEV_COOLANT, MEV_PAUSE, MEV_END, MEV_HOMED, MEV_HOME_FAIL } mev_t;

typedef struct {
	fpga_t *f;
	int64_t steps[NAXES];        /* commanded steps (sum of all bursts) */
	double pos[NAXES];           /* commanded machine position, mm */
	/* op queue + its plan (parallel arrays) */
	gc_op_t q[MQ_LEN]; int qh, qn;
	struct plan {
		bool line, rapid;
		double u[NAXES], len;
		double vnom, acc, jerk;  /* nominal speed (mm/s, before override), limits along the path */
		double vcap;             /* highest path speed the axes / FPGA sustain (caps feed x override) */
		double aplan;            /* effective acceleration for planning (S-curve average) */
		double vj;               /* corner limit at the START of this segment (geometry only) */
		double vb, ve;           /* backward-pass entry speed, final planned entry speed */
	} pl[MQ_LEN], cur;           /* cur = the segment being executed */
	double plan_end[NAXES];      /* end point of the last queued line */
	double qdist;                /* length of the queued lines */
	int replan_from;             /* lowest queue index whose plan is stale, -1 = none */
	uint32_t replan_iters;       /* planner work counter (perf) */
	int last_ov;
	double a;                    /* current acceleration along the path (S-curve state) */
	double applied_pct;          /* actual speed / programmed speed x 100, -1 when not cutting */
	/* current op */
	bool active; gc_op_t op;
	double start[NAXES], u[NAXES], len, s, v, vmax, acc;
	double dwell_left;
	/* control */
	bool hold, stopping;
	bool dry;
	uint8_t hw_busy;             /* FPGA axes still active/stepping (read once per tick) */                    /* simulate: positions advance, no bursts reach the FPGA */
	int feed_ov, spin_ov;        /* % ; rapid % = min(feed %, 100) */
	double nudge[NAXES];         /* pendant: distance still to move per axis */
	int jog_axis, jog_dir;       /* continuous jog, jog_dir 0 = off */
	int last_dir[NAXES];
	int burst_dir[NAXES];         /* direction of the last burst sent to the FPGA */
	uint32_t deferred;           /* reversals held back until the axis stopped stepping */
	int line;                    /* source line of the op being executed */
	/* homing */
	struct { bool on; int k, phase, axis; double travel; } home;
	bool (*switch_active)(int axis);
	bool (*range)(int axis, double *lo, double *hi);   /* soft/bench limits for jogs + nudges */
	int soft_stop;               /* axis+1 when a jog braked to a standstill at its limit */
	/* events for the controller */
	mev_t ev; gc_op_t ev_op;
} motion_t;

void motion_init(motion_t *m, fpga_t *f, const double mpos[NAXES]);
void motion_tick(motion_t *m, double dt);
bool motion_push(motion_t *m, const gc_op_t *op);     /* false if the queue is full */
int motion_space(const motion_t *m);
bool motion_idle(const motion_t *m);                   /* nothing queued, nothing moving */
bool motion_wants_more(const motion_t *m);             /* lookahead not yet deep enough */
void motion_move_to(motion_t *m, const double target[NAXES], double feed, bool rapid);
void motion_jog_cont(motion_t *m, int axis, int dir); /* dir 0 = stop (controlled) */
void motion_nudge(motion_t *m, int axis, double mm);
void motion_hold(motion_t *m, bool on);
void motion_stop(motion_t *m, bool immediate);        /* controlled: decel, then flush */
void motion_home(motion_t *m);
void motion_set_pos(motion_t *m, int axis, double mm);
/* where the machine will be once everything queued has run (relative jogs chain from here) */
void motion_queued_end(const motion_t *m, double out[NAXES]);
int rapid_override(int feed_ov);                       /* the rapid-follows-feed rule */

#endif
