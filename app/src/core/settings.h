/* Settings: one table (SETTINGS[]) describes every value - key, label, group, type,
 * limits, decimals, lock rules. The settings screen, the API and the file format all
 * use that table, so a setting exists in exactly one place.
 * Defaults are this machine's real saved config (docs/machine-settings.md). */
#ifndef SETTINGS_H
#define SETTINGS_H
#include "../common.h"

typedef struct {
	double steps_per_mm, max_speed, accel;       /* mm/s, mm/s² (speeds in mm/s since 2026-11-01, user: "mm/min is hard to compare") */
	double jerk;                                 /* mm/s³: how fast acceleration may change (S-curve) */
	double corner_speed;                         /* mm/s: speed change allowed instantly at a corner */
	double soft_min, soft_max;
	double bench_min, bench_max;                 /* bench limits: a work box, active even un-homed */
	double home_search, home_locate, home_pos, home_backoff;
	int home_dir;                                /* 0 = negative, 1 = positive */
	bool dir_invert, pulse_high, enabled;
	int limit_port, home_port, alarm_port;       /* INnn, 0 = none ("-"); home_port only without home_combined */
	bool limit_level_high;                       /* this axis' limit switch level, only with limit_level_per_axis */
	bool home_level_high;                        /* ... and its home switch's, only when the switches are not combined */
	int output;                                  /* pulse/dir connector: 0 = none ("-"), 1..4 = X Y Z A (XP/XD ...) */
} axis_set_t;

typedef struct {
	axis_set_t ax[NAXES];
	int home_order;            /* index into HOME_ORDERS */
	bool soft_limits;
	bool retain_homed;         /* homed survives a restart / power cut if the machine was at rest (off by default) */
	bool bench_limits;         /* the bench box (axis.*.bench_min/max) is enforced, homed or not */
	bool limit_level_high;     /* vendor "active electric level High" = active when open */
	bool limit_level_per_axis; /* 2026-11-02: each axis has its own level (axis.*.level_high) instead of the one above */
	bool home_combined;        /* 2026-11-02: one switch per axis for home and limit (axis.*.limit_port); off = homing reads axis.*.home_port */
	int estop_port; bool estop_level_high;
	bool pendant_estop;
	double spindle_max, spindle_default, spindle_delay;
	int out_spindle, out_flood, out_aux;         /* OUT1..3 for M3 / M8 / M10, 0 = none ("-") */
	int out_enable;            /* OUT1..3: drive enable for all axes, on while the drives are on; 0 = none */
	int enable_level;          /* the enable output's level while the drives are on: 0 low (sinking = OUT on), 1 high */
	double safe_z, jog_speed, default_feed;      /* jog_speed mm/s; default_feed mm/min (a G-code F) */
	int lookahead;             /* planner: at least this many segments ahead */
	double junction_dev;       /* mm: corner tolerance (0 = only the per-axis corner jump) */
	bool graceful_stop;        /* Reset / E-stop / limit brake along the path at max deceleration */
	int pos_dec, tool_dec, view_iso;
	double edge_dia;
	bool pendant; int quick_ms, wheel_div;
	double jog_jerk;           /* jogs + wheel: 0 = acceleration only (trapezoid, user 2026-10-31), else their own jerk */
	int pendant_follow;        /* 0 exact: every click is moved; 1 velocity: the axis stops ~100 ms after the wheel, surplus dropped */
	int pendant_smooth_ms;     /* wheel speed = distance still queued / this time (0 = full speed per click) */
	int approach_pct;          /* % of max_speed allowed towards a limit inside the approach zone (bench..soft, >= 10 mm) */
	int queue_ms;              /* motion kept queued in the FPGA = jog/wheel latency, absorbs loop jitter */
	bool wheel_reverse;        /* flip the raw count: on this pendant clockwise counts DOWN */
	bool api; int api_port; bool remote_start; int start_delay; bool agent_msgs;
	/* what the API may do, each on its own (panel only; user 2026-10-24: whatever can hurt
	 * someone at the machine is off by default - start, jog, moves, spindle, coolant, keys) */
	bool api_programs, api_offsets, api_tools, api_settings, api_panel, api_overrides;
	bool api_jog, api_moves, api_spindle, api_coolant, api_drives, api_keys;
	double park[3], tc[3];     /* macros: machine X Y Z of the park and tool-change positions */
	double warm_rpm, warm_s;   /* macros: spindle warm-up */
	int tc_mode;               /* M6: 0 pause + change by hand, 1 tool-change macro, 2 ignore */
	int tc_where;              /* by hand: 0 Z up to safe height where it is, 1 the tool-change position */
	double tc_return_pct;
	int gc_dwell_ms;           /* G4 P / cycle P: 0 seconds, 1 milliseconds (Fanuc) */
	int gc_tlo;                /* G43/G44: 0 ignored (Z touched off per tool), 1 the Tools table length */
	int gc_cutcomp;            /* G41/G42: 0 refuse the program, 1 ignore (path already compensated in CAM) */
	bool gc_block_delete;      /* skip blocks that start with / */
	int job_order;             /* several JOB parts: 0 one tool at a time (all parts per tool), 1 one part at a time */      /* back to the program after the change: % of rapid */
} settings_t;

/* ST_PORT: a connector choice (INnn, OUTn, a pulse/dir output) stored as an index into opts,
 * opts[0] = "-" = not used. Shown and sent over the API by name, never as a bare 0. */
typedef enum { ST_DBL, ST_INT, ST_BOOL, ST_ENUM, ST_PORT } st_type_t;
enum { DEC_POS = -1, DEC_TOOL = -2 };

typedef struct {
	const char *key, *group, *label, *unit;
	st_type_t type;
	size_t off;
	double min, max;
	int dec;                    /* decimals, or DEC_POS / DEC_TOOL */
	const char *const *opts;    /* ST_ENUM labels, NULL-terminated */
	bool machine;               /* locked while anything moves */
	bool api_ro;                /* not writable over the API (remote start) */
} setting_desc_t;

extern settings_t SET;
extern const setting_desc_t SETTINGS[];
extern const int NSETTINGS;
extern const char *const SETTING_GROUPS[];
extern const char *const HOME_ORDERS[];

/* the input an axis homes on and the level its switches are read at, after the two flags */
int axis_home_port(int a);
bool axis_level_high(int a);                  /* the limit switch (and the shared switch) */
bool axis_home_level_high(int a);             /* the home switch */

void settings_defaults(settings_t *s);
int settings_load(const char *path);          /* 0 ok, -1 missing (defaults kept) */
int settings_save(const char *path);
double setting_get(const setting_desc_t *d);
void setting_set(const setting_desc_t *d, double v);   /* clamps */
const setting_desc_t *setting_find(const char *key);
int setting_decimals(const setting_desc_t *d);
void setting_format(const setting_desc_t *d, char *buf, size_t n);
/* a port or choice by name ("-", "IN02", "OUT1", "A" or "A (AP/AD)", "high"; case-insensitive) or number; false = unknown */
bool setting_parse_port(const setting_desc_t *d, const char *s, double *v);
const char *setting_port_name(const setting_desc_t *d, int v);   /* short name for the API: "-", "IN02", "A" */
/* why value v can't be used for d (an output already in use by another function), or NULL */
const char *setting_conflict(const setting_desc_t *d, double v);
const char *setting_hint(const char *key);    /* F5 on the Settings screen; NULL = none (a test keeps that from happening) */

/* ---- work offsets and tools (persisted separately: they change daily) ---- */
#define NWCS 6
#define NTOOLS 12
typedef struct { double off[NAXES]; bool job; } wcs_t;
typedef struct { double dia, len; char note[24]; } tool_t;
typedef struct {
	wcs_t wcs[NWCS];          /* machine position of each G54..G59 zero */
	int active;
	tool_t tool[NTOOLS];      /* T1..T12 */
	int loaded_tool;          /* 0 = none */
} workdata_t;

extern workdata_t WD;
void workdata_defaults(workdata_t *w);
int workdata_load(const char *path);
int workdata_save(const char *path);

#endif
