/* Motion: turns operations into FPGA motion blocks, one 10 ms block at a time.
 *
 * Software interpolation with a lookahead planner: the queue holds at least SET.lookahead
 * segments AND at least the braking distance from top speed (so runs of tiny engraving or
 * arc segments still see far enough). Corner speeds come from a per-axis "corner jump"
 * (the speed change an axis may take instantly) plus a junction-deviation tolerance; a
 * backward pass makes every segment slow down in time for what follows, a forward pass
 * limits it to what acceleration can reach. Execution is an S-curve: acceleration itself
 * ramps with the per-axis jerk. Every 10 ms of planned motion becomes one block carrying all
 * four axes, pushed into the FPGA's own block queue, which plays the blocks back to back
 * (docs/block-mode.md). The old step-burst emitter was removed 2026-11-02. */
#ifndef MOTION_H
#define MOTION_H
#include "gcode.h"
#include "../hal/fpga.h"

#define MQ_LEN 512

typedef enum { MEV_NONE, MEV_SPINDLE, MEV_COOLANT, MEV_PAUSE, MEV_END, MEV_HOMED, MEV_HOME_FAIL } mev_t;

typedef struct {
	fpga_t *f;
	int64_t steps[NAXES];        /* commanded steps (sum of all blocks sent) */
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
	bool dry;                    /* simulate: positions advance, nothing reaches the FPGA */
	uint8_t hw_busy;             /* FPGA axes still stepping (read once per tick) */
	int feed_ov, spin_ov;        /* % ; rapid % = min(feed %, 100) */
	/* blocks (2026-11-01, docs/block-mode.md): the FPGA's queue is the clock */
	int q_est;                   /* blocks queued in the FPGA as of this tick (read once + sent since) */
	int feed_ahead_ms;           /* main loop: fill the queue up to this much before a long frame (one tick) */
	uint32_t blocks;             /* blocks sent (perf) */
	bool stop_resync;            /* an immediate stop flushed the queue: adopt the counters silently at rest */
	int8_t fpga_mode;            /* 1 = the FPGA was put in block mode, -1 = not yet (after fpga_init) */
	int stuck_ticks; bool fpga_stuck;   /* blocks queued but nothing plays: after 300 ms a fault (the controller reports it once) */
	bool fpga_halted;            /* sticky: the FPGA's queue reads non-empty with nothing playing (only a power cycle clears the FPGA; this clears when the queue reads empty again) */
	double nudge[NAXES];         /* pendant: distance still to move per axis */
	double nudge_in[NAXES];      /* pendant: distance clicked in since the last slice */
	double wheel_v[NAXES];       /* pendant: the wheel's own speed, mm/s, averaged over ~80 ms (feedforward) */
	int jog_axis, jog_dir;       /* continuous jog, jog_dir 0 = off */
	double jv[NAXES], ja[NAXES]; /* jog / wheel S-curve state per axis: speed mm/s, accel mm/s² */
	int last_dir[NAXES];
	int line;                    /* source line of the op being executed */
	/* homing */
	struct { bool on; int k, phase, axis; double travel; } home;
	bool (*switch_active)(int axis);
	bool (*range)(int axis, double *lo, double *hi);   /* soft/bench limits for jogs + nudges */
	/* the controller's level gate: true = no steps on this axis this way (E-stop, drives off,
	 * a tripped limit switch). Not asked while braking or homing, which handle switches
	 * themselves. NULL = never blocked. */
	bool (*axis_blocked)(int axis, int dir);
	int soft_stop;               /* axis+1 when a jog braked to a standstill at its limit */
	/* safety (2026-10-31: a steps/mm change once streamed 336 mm of X at full speed) */
	double spm[NAXES];           /* the steps/mm that steps[] and pos[] were counted in */
	uint8_t rescaled;            /* axes whose steps/mm changed: position rebased, homing lost */
	int runaway;                 /* axis+1: a step delta no plan could produce was refused */
	uint32_t blocked;            /* moves held back by axis_blocked */
	double emit_pos[NAXES];      /* pos at the previous emit */
	double stop_left;            /* s: a controlled stop must be at rest within this, else cut dead */
	bool stop_cut;               /* a controlled stop overran its time bound and was cut */
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
/* the planner's own segment geometry and corner limit (also used by the time estimate) */
bool motion_plan_setup(struct plan *p, const gc_op_t *op, const double start[NAXES]);
double motion_corner_limit(const struct plan *A, const struct plan *B);

#endif
