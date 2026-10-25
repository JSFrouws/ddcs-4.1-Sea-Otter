/* A loaded G-code program: text, line index, and a decimated preview polyline in
 * WORK coordinates (so it doesn't depend on which WCS it will run on). */
#ifndef PROGRAM_H
#define PROGRAM_H
#include "../common.h"

#define PREVIEW_MAX 120000
#define PROG_MAX_TC 64           /* tool changes remembered (multi-part jobs cut per tool) */

typedef struct { float x, y, z; int32_t line; uint8_t rapid; } pv_t;

typedef struct {
	char name[128], path[256];
	char *text; size_t len;
	int nlines;
	uint32_t *line_off;          /* byte offset of each line */
	pv_t *pv; int npv;           /* preview vertices; segment i = pv[i-1] -> pv[i] */
	/* the preview is in work coordinates of the WCS active at load (pv_off = its machine
	 * offset); pv_wcs0 = the WCS the program itself starts in. G54-G59 switches inside the
	 * program are already in it (each move went through its own offset). */
	double pv_off[3]; int pv_wcs0;
	bool multi_wcs;              /* it switches between G54-G59 itself: no multi-part job (they would mix) */
	float lo[3], hi[3];          /* work-coordinate bounding box of cutting moves (view) */
	float mlo[3], mhi[3];        /* ... of every move, rapids included (soft/bench limit check) */
	double est_s;                /* run time at 100 %, margin included (core/estimate.h); 0 until est_cum is there */
	float *est_cum;              /* seconds before each line, per override level: [line * EST_NLV + level];
	                                NULL while it is still being worked out */
	float *exact_cum;            /* seconds before each line at 100 %, from replaying the program through
	                                the real motion executor (dry): exact; NULL until that is done */
	uint32_t gen;                /* load counter: which program this is */
	/* tool changes: the M6 lines split the program into tool segments; segment 0 is before the
	 * first M6, segment i starts at tc_line[i-1] (the M6 line itself) */
	int ntc; int tc_line[PROG_MAX_TC]; int tc_tool[PROG_MAX_TC];
	/* good places to go to (Run, Shift + F5): the program's sections (comment lines that
	 * start a block, like CAM operation names) or, without those, every retract to the
	 * safe height it uses most (the highest Z that occurs often) */
	int *goto_line; int ngoto; bool goto_sections; float goto_z;
	char err[128]; int err_line;
	char warn[128];
	bool loaded;
} program_t;

int program_load(program_t *p, const char *path);           /* 0 ok, -1 error (p->err) */
int program_load_text(program_t *p, const char *name, const char *text, size_t len);
void program_free(program_t *p);
/* copy line n (0-based) into buf, trimmed */
void program_line(const program_t *p, int n, char *buf, size_t sz);

#endif
