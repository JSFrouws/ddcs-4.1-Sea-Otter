/* Machine state machine + controller.
 *
 * act(cmd, arg, src) is the ONLY entry point that moves the machine or changes its
 * state. Panel keys, the pendant, the Ethernet API and internal events (limit switch,
 * E-stop, motion finished) all call it. It checks a state table (ALLOW) plus a few
 * extra guards, runs the command's effect, then the transition (NEXT). A refused
 * command changes nothing and returns the reason. See docs/operator-procedures.md. */
#ifndef MACHINE_H
#define MACHINE_H
#include "motion.h"
#include "program.h"
#include "settings.h"

/* M_TOOL: a program waits at an M6 for the tool to be changed by hand - jogging and
 * touch-off are allowed, Start (twice: the return move is confirmed) goes back and continues */
typedef enum { M_ESTOP, M_OFF, M_IDLE, M_MOVE, M_HOMING, M_PROBING, M_RUN, M_HOLD, M_LIMIT, M_TOOL, M__N } mstate_t;
typedef enum {
	C_JOG, C_TOUCHOFF, C_GOTOZERO, C_HOME, C_PROBE, C_START, C_PAUSE, C_STOP, C_SPINDLE,
	C_DRIVES_ON, C_DRIVES_OFF, C_LOAD, C_EDIT_TABLE, C_EDIT_SETTING, C_OVERRIDE,
	C_ESTOP_IN, C_ESTOP_OUT, C_LIMIT_HIT, C_LIMIT_CLEAR, C_DONE, C_MACRO, C_COOLANT, C_TOOL_READY, C__N
} cmd_t;
typedef enum { SRC_PANEL, SRC_API, SRC_MACHINE } src_t;
enum { ARM_RUN, ARM_TRYCUT, ARM_SIM };

typedef struct {
	int axis, dir;          /* jog / touch-off / limit */
	double dist;            /* jog step (mm); 0 with cont = continuous */
	                        /* kind 1 with C_GOTOZERO: Z up to the safe height only */
	bool cont;
	int kind, delta;        /* override: kind 0 feed, 1 spindle; delta in % */
	const char *path;       /* load */
	const char *text; size_t len;  /* load from API upload */
} arg_t;

#define AGENT_STEPS 8
typedef struct {
	bool on;
	char title[64], who[32];
	struct { char text[96]; char check[24]; bool done; } step[AGENT_STEPS];
	int n;
} agent_t;

typedef struct {
	mstate_t m;
	bool homed;
	int limit_axis, limit_dir;     /* dir 0 = unknown (switch active since power-up) */
	bool lim_active[NAXES];
	int lim_dir[NAXES];            /* per axis: 0 free, +1/-1 tripped moving that way, 2 = active, direction unknown */
	bool spindle; double rpm; int coolant;
	int armed, run_mode;
	program_t prog;
	gc_t gc; bool feeding;
	/* multi-part job (several WCS marked JOB): each tool segment of the program (between M6
	 * lines) is cut on every JOB part in turn, then the next tool - one tool change per tool */
	int job_idx, job_seg;
	/* tool change at an M6 (Settings > Tool change). The feeder waits at the M6 (feed_wait)
	 * until it has been dealt with: by hand (TC_GOING to the change position, then M_TOOL),
	 * by a macro (TC_MACRO: its moves run as part of the program), or skipped */
	enum { TC_NONE, TC_GOING, TC_MACRO } tc_state;
	int tc_tool;
	bool tc_confirm;                /* Start pressed once in M_TOOL: the return move is shown */
	bool feed_wait;
	double tc_off[NWCS][NAXES];     /* work offsets when the change began (Z touch-off = new tool length) */
	char *tc_text; gc_t tc_gc;      /* the tool-change macro, expanded */
	int resume_line, work_done;
	int run_from;                   /* the line the running program started (or resumed) at */
	uint64_t rewind_ask;            /* Reset pressed once to go back to line 1: when (it needs a second press) */
	int touch_axis, touch_dir;     /* last touch-off (same key again = 0) */
	bool touched[NAXES];
	int countdown; uint64_t countdown_t;
	char msg[160]; bool msg_bad; uint64_t msg_t;
	char pause_msg[96];
	bool hold_move;                 /* HOLD of a move (Goto 0, Safe Z), not of a program: Start resumes it */
	char move_name[48];             /* the move in progress, shown on the Run page */
	motion_t mo;
	fpga_t fp;
	agent_t agent;
	uint32_t inputs;
	/* an armed / running macro: it is C.prog for that time, the operator's program waits in
	 * saved_prog and comes back when the macro ends, is reset or stopped (core/macro.h) */
	bool macro_on;
	char macro_name[48], macro_path[256];
	program_t saved_prog;
	int saved_resume, saved_done, saved_armed;
	char stop_cause[96];            /* why the drives are off / E-stop is active, shown on the panel */
	char driver_warn[200];          /* set at start-up when motiondev.ko is not the tested one */
	int32_t fpos[NAXES];            /* FPGA position counters (latched every 20 ms) */
	int32_t pos_err[NAXES];         /* counter - commanded steps, checked when at rest */
	uint32_t pos_checks, pos_faults;
	int fault_line;                 /* program line / state when the last fault was found */
	pendant_t pend;
	bool sim;
	const char *cfg_path, *wd_path;
} ctl_t;

extern ctl_t C;
extern const char *const STATE_NAMES[M__N];
extern const char *const CMD_NAMES[C__N];

void ctl_init(bus_t *bus, bool sim, const char *cfg_path, const char *wd_path);
void ctl_tick(double dt);
/* C_MACRO arms a macro: arg path = its file (or its name). See core/macro.h */
const char *can(cmd_t c, const arg_t *a, src_t src);   /* NULL = allowed, else why not */
bool act(cmd_t c, const arg_t *a, src_t src);
bool ctl_busy(void);                                   /* RUN HOLD HOMING MOVE PROBING */
void ctl_toast(bool bad, const char *fmt, ...);
bool ctl_axis_range(int axis, double *lo, double *hi);   /* soft + bench limits in force; false = none */
double ctl_trycut_z(void);                             /* TryCut: clearance level above the work plane */
double ctl_work(int axis);                             /* work position, active WCS */
/* M_TOOL: where Start (confirmed) goes back to - machine coordinates: a straight line to
 * (x, y, via_z), then down to z when via_z is higher. Returns the speed in mm/min. */
double ctl_tc_target(double t[NAXES], double *via_z);
int ctl_prog_wcs(void);                                /* the WCS the running program uses */
void ctl_apply_axis_config(void);                      /* push settings to the FPGA */
void ctl_save(void);
int ctl_cur_line(void);

#endif
