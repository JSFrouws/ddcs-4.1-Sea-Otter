/* Panel UI: six screens driven by the 18 physical keys and the optional pendant.
 * Mode = Run <-> Jog, Page R = Run -> Files -> Work -> Tools -> Macros -> Settings, Tab = next pane,
 * Shift = ALT layer. Every machine action goes through act() (core/machine.h). */
#ifndef UI_H
#define UI_H
#include "gfx.h"
#include "../core/machine.h"

typedef enum {
	K_NONE, K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7,
	K_TAB, K_SEL, K_SHIFT, K_MOD, K_MODE, K_CANCEL, K_YP, K_ENTER, K_XM, K_YM, K_XP, K__N
} pkey_t;
extern const char *const KEY_NAMES[K__N];      /* "F1", "TAB", ... (API / host panel) */
pkey_t key_by_name(const char *s);

typedef enum { SCR_RUN, SCR_JOG, SCR_FILES, SCR_WORK, SCR_TOOLS, SCR_SETTINGS, SCR_MACROS, SCR__N } scr_t;

#define MAXFILES 64
typedef struct {
	scr_t screen;
	int pane[SCR__N];
	bool shift;
	struct {
		bool on, neg, allow_neg, locks;
		char label[64], unit[12];
		char d[12]; int n, nint, cur, maxint;
		double max;
		void (*ok)(double v, int ctx); int ctx;
	} ed;
	int macro, prog_line, files_idx, src, wcs_row, wcs_col, probe, tool_row, tool_col, grp, item, jog_field, step;
	struct { bool iso, saver; double yaw, pitch, zoom, panx, pany; } view;
	char files[MAXFILES][160]; int nfiles;
	/* pendant */
	bool held; uint64_t held_t; int held_counts, wheel_rem;
	uint64_t last_render;
	bool dirty;
} ui_t;

extern ui_t U;
extern void (*ui_yield)(void);   /* set by main: runs a motion slice if one is due (long draws call it) */
extern const double JOG_STEPS[];
extern const int NJOG_STEPS;

void ui_init(void);
void ui_key(pkey_t k);
void ui_key_release(pkey_t k);                     /* continuous jog stops on release */
void ui_pendant(const pendant_t *p, int counts);    /* call every tick */
void ui_render(fb_t *fb);
uint8_t ui_leds(void);                              /* LED_* bits for the TM1638 */
enum { LED_START = 1, LED_PAUSE = 2, LED_RESET = 4, LED_SPINDLE = 8, LED_SHIFT = 16, LED_ETH = 32, LED_USB = 64 };

/* ---- internals shared by the screen files ---- */
#define TOP_H 42
#define SK_H 52
#define SIDE_W 112
#define CONTENT_W (SCR_W - SIDE_W)
#define BODY_Y TOP_H
#define BODY_H (SCR_H - TOP_H - SK_H)

typedef struct { const char *label, *sub; } sk_t;
int ui_pane(void);                                 /* index of the focused pane */
int ui_npanes(scr_t s);
const char *ui_pane_name(scr_t s, int i);
void ui_frame(fb_t *fb, const sk_t sk[7]);          /* top bar, banner, soft keys, side strip, toast */
int ui_banner_h(void);
void fmt_pos(char *b, size_t n, double v);
void fmt_tool(char *b, size_t n, double v);
void ui_editor_open(const char *label, double v, int dec, const char *unit, int intdig, double max, bool neg, bool locks, void (*ok)(double, int), int ctx);
void ui_editor_key(pkey_t k, bool shift);
void ui_editor_turn(int n);
void ui_editor_render(fb_t *fb);
void ui_view_render(fb_t *fb, int x, int y, int w, int h, bool mini);
bool ui_view_key(pkey_t k);
void ui_scan_files(void);

/* per screen */
void run_render(fb_t *fb);  void run_key(pkey_t k, bool sh);  void run_sk(sk_t sk[7]);
void jog_render(fb_t *fb);  void jog_key(pkey_t k, bool sh);  void jog_sk(sk_t sk[7]);
void files_render(fb_t *fb); void files_key(pkey_t k, bool sh); void files_sk(sk_t sk[7]);
void work_render(fb_t *fb); void work_key(pkey_t k, bool sh); void work_sk(sk_t sk[7]);
void tools_render(fb_t *fb); void tools_key(pkey_t k, bool sh); void tools_sk(sk_t sk[7]);
void set_render(fb_t *fb);  void set_key(pkey_t k, bool sh);  void set_sk(sk_t sk[7]);
void macros_render(fb_t *fb); void macros_key(pkey_t k, bool sh); void macros_sk(sk_t sk[7]);
void ui_pendant_nav(int n);
void ui_pendant_change(int n);
void ui_pendant_click(void);

#endif
