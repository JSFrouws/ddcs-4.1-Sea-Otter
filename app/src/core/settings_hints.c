/* One short explanation per setting, shown on the Settings screen with F5 (user 2026-10-25:
 * "create hints for ALL settings"). test_ui checks that every key in SETTINGS[] has one.
 * The axis rows share a hint per kind (axis.<X|Y|Z|A>.steps ...). */
#include <stdio.h>
#include <string.h>
#include "settings.h"

typedef struct { const char *key, *hint; } hint_t;

static const hint_t HINTS[] = {
	{ "display.pos_dec", "How many decimals the position readouts show (X Y Z A). Only the display: the machine always works to the step." },
	{ "display.tool_dec", "How many decimals tool sizes show (diameter, length) in the tool table, editor and Run screen." },
	{ "display.view", "The toolpath view after a restart: 2D plan, isometric wireframe, or CPU saver (no preview, more time for motion). F4 View on the Run screen changes it for now." },
	{ "jog.edge_dia", "Diameter of the edge finder or tool you touch off with. Shift + X/Y key then sets the axis to minus/plus half of it, so the zero lands on the edge. 0: the axis becomes 0." },
	{ "jog.feed", "Speed of jogs with the panel keys and of continuous jogs, in mm/min. Capped by each axis' max speed." },
	{ "jog.safe_z", "Height above the work zero that Goto 0, Safe Z, part changes, resumes and tool changes go up to before moving across. Set it above clamps and stock." },
	{ "motion.graceful_stop", "On: Reset, E-stop and limits brake along the path at full deceleration, so no step is lost (about 1 mm run-on at full speed). Off: stop at once; the drives may lose steps." },
	{ "planner.lookahead", "How many program moves the planner looks ahead to keep speed through short segments. More = smoother on dense 3D code, slightly more CPU." },
	{ "planner.junction_dev", "How far (mm) a corner may be rounded off in speed terms: larger corners are taken faster. 0 = only the per-axis corner speed jump counts." },
	{ "gcode.default_feed", "The feed used for G1/G2/G3 moves when the program never set an F word." },
	{ "pendant.on", "Is an MPG pendant connected? Off: the wheel, dead-man and axis switch are ignored (its E-stop too)." },
	{ "pendant.quick_ms", "A press of the dead-man shorter than this, without turning the wheel, counts as Enter on the focused item." },
	{ "pendant.wheel_div", "How many raw wheel counts make one click (detent). Set it so one click is one step." },
	{ "pendant.wheel_reverse", "Flips the wheel direction, so clockwise moves + and increases values on this pendant." },
	{ "pendant.estop", "Whether the pendant's E-stop button stops the machine. Note: the motion controller itself also stops stepping while that line reads pressed." },
	{ "api.on", "The master switch for the network API on port below (and the browser panel). Off: nothing on the network can read or control this machine." },
	{ "api.port", "The TCP port of the API and the browser panel (default 8080). Takes effect after a restart." },
	{ "api.programs", "Network clients may upload and load programs, choose Run / TryCut / Simulate and rewind. Nothing moves by this alone." },
	{ "api.offsets", "Network clients may change work offsets (G54-G59), the active one, the JOB marks, and touch off." },
	{ "api.tools", "Network clients may edit the tool table and say which tool is in the spindle." },
	{ "api.settings", "Network clients may change settings and restore the defaults. The API switches themselves can only be changed here." },
	{ "api.overrides", "Network clients may change the feed and spindle override (0-300 %)." },
	{ "api.panel", "Network clients may show messages, operator checklists and switch screens on this panel." },
	{ "api.agent_msgs", "Show the checklist card a network agent sends (preparing a job) on the Run screen." },
	{ "api.remote_start", "Network clients may START programs and macros, and confirm the way back after a tool change. The panel counts down first; Reset cancels." },
	{ "api.start_delay", "Seconds the panel counts down before a remote START, so the operator sees it coming and can press Reset. 0 = no countdown." },
	{ "api.jog", "Network clients may jog the machine (fixed distances only; a continuous jog is never offered over the network)." },
	{ "api.moves", "Network clients may home the machine and run Goto 0 and Safe Z." },
	{ "api.spindle", "Network clients may switch the spindle on and off by hand." },
	{ "api.coolant", "Network clients may switch the coolant (M8 output) on and off by hand." },
	{ "api.drives", "Network clients may switch the drives ON. Switching them OFF is always allowed over the network." },
	{ "api.keys", "Network clients may press panel keys, e.g. the clickable keys of the browser panel. Off: the browser panel is a live view only." },
	{ "home.order", "Which axes home in which order. Z first keeps the tool clear of the part and clamps." },
	{ "limits.soft", "Once homed, keep every move inside the soft limits below: jogs stop at the edge, programs that would leave the box are refused at Start." },
	{ "home.retain", "Keep 'homed' after a restart or power cut, if the machine was standing still. Quick, but the drives were reset: expect 0.05-0.1 mm. Home again for precision." },
	{ "limits.bench", "Keep every move inside the bench box (axis bench limits), homed or not. Handy to protect a part of the table or a test setup." },
	{ "limits.level_high", "Electrical level of the limit / home switches. On: active when the input is open (normally closed switches, safest: a broken wire stops the axis)." },
	{ "estop.port", "Which input (IN01-IN18) the external E-stop is wired to. 0 = none." },
	{ "estop.level_high", "Electrical level of the external E-stop. On: active when open (normally closed button, safest: a broken wire stops the machine)." },
	{ "spindle.max", "The spindle speed at 10 V on the 0-10 V output. All speeds are scaled to this." },
	{ "spindle.default", "Spindle speed for M3 without an S word, and for the spindle key (F6)." },
	{ "spindle.delay", "Seconds to wait after the spindle starts (M3/M4) before the program moves on, so it is up to speed before cutting." },
	{ "out.spindle", "Which output relay (OUT1-OUT3) switches the spindle for M3/M4. 0 = none." },
	{ "out.flood", "Which output relay switches the coolant for M8 (and Shift + F6). 0 = none." },
	{ "out.aux", "Which output relay switches the auxiliary output for M10 / M11. 0 = none." },
	{ "macro.park_x", "Machine X of the park position (Park macro). Needs a homed machine." },
	{ "macro.park_y", "Machine Y of the park position (Park macro)." },
	{ "macro.park_z", "Machine Z the Park macro ends at (it goes up first)." },
	{ "tool.mode", "What an M6 in a program does: stop and let you change the tool by hand, run a tool-change macro (toolchange.nc / toolchange-T<n>.nc), or nothing." },
	{ "tool.where", "Where a change by hand happens: Z up to the safe height where the program is, or at the tool-change position below (needs a homed machine)." },
	{ "tool.return_pct", "Speed of the straight move back to the program after a tool change, as % of rapid." },
	{ "macro.tc_x", "Machine X of the tool-change position (tool change by hand, and the Tool change position macro)." },
	{ "macro.tc_y", "Machine Y of the tool-change position." },
	{ "macro.tc_z", "Machine Z of the tool-change position (it goes up first, then across, then to this height)." },
	{ "macro.warm_rpm", "Spindle speed of the Spindle warm-up macro. 0 = no warm-up." },
	{ "macro.warm_s", "How long the Spindle warm-up macro runs, in seconds." },
	/* axis rows, by kind */
	{ "axis.*.steps", "Steps per mm (per degree for A): motor steps x microsteps / screw pitch. 5 mm screw, 200 x 50 = 2000. Wrong values scale every move." },
	{ "axis.*.max_vel", "Highest speed of this axis (rapids, jogs), mm/min. The motion controller tops out at 40 000 steps/s." },
	{ "axis.*.accel", "How fast this axis may speed up and slow down, mm/s². Too high: the drive loses steps; too low: slow corners." },
	{ "axis.*.jerk", "How fast the acceleration itself may change (S-curve), mm/s³. Lower = softer starts and stops, a bit slower." },
	{ "axis.*.corner_jump", "The speed change this axis may make instantly at a corner, mm/min. Higher = faster corners, more shock on the mechanics." },
	{ "axis.*.dir_invert", "Flips the direction signal, so + moves the right way." },
	{ "axis.*.soft_min", "Lowest machine position this axis may go to once homed (soft limit -)." },
	{ "axis.*.soft_max", "Highest machine position this axis may go to once homed (soft limit +)." },
	{ "axis.*.bench_min", "Lowest position of the bench box (when Bench limits is on), homed or not." },
	{ "axis.*.bench_max", "Highest position of the bench box (when Bench limits is on), homed or not." },
	{ "axis.*.limit_port", "The input the shared limit / home switch of this axis is wired to. 0 = none." },
	{ "axis.*.home_dir", "Which way this axis moves to find its home switch." },
	{ "axis.*.home_pos", "The machine position this axis gets at its home switch." },
	{ "axis.*.home_backoff", "How far the axis moves back off its switch after homing, so the switch is free again." },
};

const char *setting_hint(const char *key)
{
	char k[64];
	if (!strncmp(key, "axis.", 5) && strlen(key) > 7 && key[6] == '.') {   /* axis.X.steps -> axis.*.steps */
		snprintf(k, sizeof k, "axis.*.%s", key + 7);
		key = k;
	}
	for (size_t i = 0; i < ARRAY_LEN(HINTS); i++) if (!strcmp(HINTS[i].key, key)) return HINTS[i].hint;
	return NULL;
}
