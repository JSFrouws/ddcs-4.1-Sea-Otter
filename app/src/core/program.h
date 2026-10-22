/* A loaded G-code program: text, line index, and a decimated preview polyline in
 * WORK coordinates (so it doesn't depend on which WCS it will run on). */
#ifndef PROGRAM_H
#define PROGRAM_H
#include "../common.h"

#define PREVIEW_MAX 120000

typedef struct { float x, y, z; int32_t line; uint8_t rapid; } pv_t;

typedef struct {
	char name[128], path[256];
	char *text; size_t len;
	int nlines;
	uint32_t *line_off;          /* byte offset of each line */
	pv_t *pv; int npv;           /* preview vertices; segment i = pv[i-1] -> pv[i] */
	float lo[3], hi[3];          /* work-coordinate bounding box of cutting moves (view) */
	float mlo[3], mhi[3];        /* ... of every move, rapids included (soft/bench limit check) */
	double est_s;                /* time estimate at 100 % */
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
