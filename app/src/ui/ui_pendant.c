/* Pendant (optional - every action also has a panel key):
 *   wheel, hold released ........ MOVE: cursor / list focus / turn the view, never changes values
 *   wheel, hold, axis OFF ........ CHANGE: digit, field value, tilt, program scrub (X1/X10/X100);
 *                                  while running: X1 feed %, X10 spindle %, X100 feed % ×10
 *   wheel, hold, axis X/Y/Z/4 .... JOG 0.001 / 0.010 / 0.100 mm per detent (state machine permitting)
 *   quick press of hold (< quick_ms, no turning, axis OFF) = Enter on the focused item
 * Positions 5 and 6 of the axis switch have no wires: they read exactly like OFF. */
#include "ui.h"

static void nav_key(int n, pkey_t up, pkey_t down) { ui_key(n > 0 ? down : up); }

void ui_pendant_nav(int n)
{
	if (U.ed.on) { ui_editor_key(n > 0 ? K_XP : K_XM, false); U.dirty = true; return; }
	extern bool set_inline_active(void);
	if (set_inline_active()) { ctl_toast(false, "Hold and turn to change the value, quick press to accept"); return; }
	int p = ui_pane();
	if (U.screen == SCR_JOG && p == 0) { ctl_toast(false, "Hold the dead-man and pick an axis to jog"); return; }
	if ((U.screen == SCR_RUN && p == 0) || (U.screen == SCR_FILES && p == 1)) { ui_view_key(n > 0 ? K_XP : K_XM); U.dirty = true; return; }
	nav_key(n, K_YP, K_YM);
}

void ui_pendant_change(int n)
{
	U.dirty = true;
	if (U.ed.on) { ui_editor_turn(n); return; }
	if (C.m == M_RUN || C.m == M_HOLD) {
		int mult = C.pend.mult;
		arg_t a = { .kind = mult == 10 ? 1 : 0, .delta = n * (mult == 100 ? 10 : 2) };   /* 2 % steps (x100: 10 %) */
		act(C_OVERRIDE, &a, SRC_PANEL);
		ctl_toast(false, a.kind ? "Spindle %d%%" : "Feed %d%%  ·  rapid %d%%", a.kind ? C.mo.spin_ov : C.mo.feed_ov, rapid_override(C.mo.feed_ov));
		return;
	}
	int p = ui_pane();
	if ((U.screen == SCR_RUN && p == 0) || (U.screen == SCR_FILES && p == 1)) { ui_view_key(n > 0 ? K_YP : K_YM); return; }
	if (U.screen == SCR_RUN && p == 1) {
		int step = C.pend.mult;
		U.prog_line = CLAMP(U.prog_line + n * step, 0, C.prog.nlines ? C.prog.nlines - 1 : 0);
		return;
	}
	if ((U.screen == SCR_JOG && p == 1) || (U.screen == SCR_SETTINGS && p == 1) || U.screen == SCR_WORK || U.screen == SCR_TOOLS) {
		if (U.screen == SCR_SETTINGS && p == 1) {
			extern bool set_inline_active(void);
			if (set_inline_active()) ui_key(n > 0 ? K_XP : K_XM);                       /* change the value */
			else for (int i = 0; i < (C.pend.mult >= 10 ? 10 : 1) && ui_pane() == 1; i++) nav_key(n, K_YP, K_YM);   /* fast scroll, stops at the edge */
			return;
		}
		ui_key(n > 0 ? K_XP : K_XM);                                                        /* field / column */
		return;
	}
	for (int i = 0; i < (C.pend.mult >= 10 ? 10 : 1); i++) nav_key(n, K_YP, K_YM);           /* fast scroll */
}

void ui_pendant_click(void)
{
	if (U.ed.on) { ui_editor_key(K_ENTER, false); U.dirty = true; return; }
	int p = ui_pane();
	if (U.screen == SCR_JOG && p == 0) return;
	bool sel = (U.screen == SCR_RUN) || (U.screen == SCR_FILES && p == 0) || (U.screen == SCR_WORK && p == 1);
	ui_key(sel ? K_SEL : K_ENTER);
}

void ui_pendant(const pendant_t *pd, int counts)
{
	if (!SET.pendant) return;
	int div = SET.wheel_div > 0 ? SET.wheel_div : 4;
	if (SET.wheel_reverse) counts = -counts;     /* clockwise = +, everywhere: jog, values, lists */
	U.wheel_rem += counts;
	int detents = U.wheel_rem / div;
	U.wheel_rem -= detents * div;
	static int held_axis = -1;                   /* the switch is only readable while held */
	if (pd->held && !U.held) { U.held = true; U.held_t = now_ms(); U.held_counts = 0; }
	if (pd->held) held_axis = pd->axis;
	if (detents) {
		if (U.held) U.held_counts += detents > 0 ? detents : -detents;
		if (!pd->held) ui_pendant_nav(detents);
		else if (pd->axis >= 0) {
			arg_t a = { .axis = pd->axis, .dir = detents > 0 ? 1 : -1, .kind = 1,
				.dist = (detents > 0 ? detents : -detents) * (pd->mult == 100 ? 0.1 : pd->mult == 10 ? 0.01 : 0.001) };
			act(C_JOG, &a, SRC_PANEL);
		} else ui_pendant_change(detents);
		U.dirty = true;
	}
	if (!pd->held && U.held) {
		U.held = false;
		if (now_ms() - U.held_t < (uint64_t)SET.quick_ms && U.held_counts == 0 && held_axis < 0) ui_pendant_click();
	}
}
