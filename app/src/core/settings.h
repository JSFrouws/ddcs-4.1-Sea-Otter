/* Settings: one table (SETTINGS[]) describes every value - key, label, group, type,
 * limits, decimals, lock rules. The settings screen, the API and the file format all
 * use that table, so a setting exists in exactly one place.
 * Defaults are this machine's real saved config (docs/machine-settings.md). */
#ifndef SETTINGS_H
#define SETTINGS_H
#include "../common.h"

typedef struct {
	double steps_per_mm, max_vel, accel;         /* mm/min, mm/s² */
	double jerk;                                 /* mm/s³: how fast acceleration may change (S-curve) */
	double corner_jump;                          /* mm/min: speed change allowed instantly at a corner */
	double soft_min, soft_max;
	double bench_min, bench_max;                 /* bench limits: a work box, active even un-homed */
	double home_search, home_locate, home_pos, home_backoff;
	int home_dir;                                /* 0 = negative, 1 = positive */
	bool dir_invert, pulse_high, enabled;
	int limit_port, home_port, alarm_port;       /* INnn, 0 = none */
} axis_set_t;

typedef struct {
	axis_set_t ax[NAXES];
	int home_order;            /* index into HOME_ORDERS */
	bool soft_limits;
	bool retain_homed;         /* homed survives a restart / power cut if the machine was at rest (off by default) */
	bool bench_limits;         /* the bench box (axis.*.bench_min/max) is enforced, homed or not */
	bool limit_level_high;     /* vendor "active electric level High" = active when open */
	int estop_port; bool estop_level_high;
	bool pendant_estop;
	double spindle_max, spindle_default, spindle_delay;
	int out_spindle, out_flood, out_aux;         /* OUT1..3 for M3 / M8 / M10, 0 = none */
	double safe_z, jog_feed, default_feed;
	int lookahead;             /* planner: at least this many segments ahead */
	double junction_dev;       /* mm: corner tolerance (0 = only the per-axis corner jump) */
	bool graceful_stop;        /* Reset / E-stop / limit brake along the path at max deceleration */
	int pos_dec, tool_dec, view_iso;
	double edge_dia;
	bool pendant; int quick_ms, wheel_div;
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
	double tc_return_pct;      /* back to the program after the change: % of rapid */
} settings_t;

typedef enum { ST_DBL, ST_INT, ST_BOOL, ST_ENUM } st_type_t;
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

void settings_defaults(settings_t *s);
int settings_load(const char *path);          /* 0 ok, -1 missing (defaults kept) */
int settings_save(const char *path);
double setting_get(const setting_desc_t *d);
void setting_set(const setting_desc_t *d, double v);   /* clamps */
const setting_desc_t *setting_find(const char *key);
int setting_decimals(const setting_desc_t *d);
void setting_format(const setting_desc_t *d, char *buf, size_t n);
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
