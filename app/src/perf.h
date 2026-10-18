/* Loop timing counters (reported in GET /api/v1/status as "perf"): where the CPU time goes
 * on the ARM926, and how much motion time the main loop's dt clamp throws away. */
#ifndef PERF_H
#define PERF_H
#include <stdint.h>

typedef struct {
	uint32_t loops;
	uint32_t tick_us_avg, tick_us_max;      /* ctl_tick (planner, motion, FPGA I/O) */
	uint32_t render_us_avg, render_us_max;  /* ui_render + display_present */
	uint32_t draw_us_avg, present_us_avg;   /* the two halves of a render */
	uint32_t gap_ms_max;                    /* longest real time between two ticks */
	uint64_t motion_ms, lost_ms;            /* motion time given to ticks / real time lost to the clamp */
} perf_t;

extern perf_t PERF;
uint64_t now_us(void);

#endif
