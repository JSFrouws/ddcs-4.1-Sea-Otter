/* Loop timing counters (reported in GET /api/v1/status as "perf"): where the CPU time goes
 * on the ARM926, and how much motion time the main loop's dt clamp throws away. */
#ifndef PERF_H
#define PERF_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t loops;
	uint32_t tick_us_avg, tick_us_max;      /* ctl_tick (planner, motion, FPGA I/O) */
	uint32_t render_us_avg, render_us_max;  /* ui_render + display_present */
	uint32_t draw_us_avg, present_us_avg;   /* the two halves of a render */
	uint32_t gap_ms_max;                    /* longest real time between two ticks */
	uint64_t motion_ms, lost_ms;            /* motion time given to ticks / real time lost to the clamp */
	uint32_t stalls;                        /* loop sections over 100 ms, named on stderr (STALL_WATCH) */
	uint32_t tick_us_last;                  /* the last ctl_tick, for the trace's tick_us column */
	uint32_t wakes;                         /* slices brought forward to a burst's end (motion_t.burst_end_us) */
	bool debug;                             /* GET /api/v1/debug?loop=1: every slice, burst, wake and frame on stderr */
} perf_t;

extern perf_t PERF;
uint64_t now_us(void);

/* Which part of the loop held the machine up: anything over 100 ms (a motion slice is 10)
 * is named on stderr. The pendant trace showed 550 ms holes it could not attribute. */
#include <stdio.h>
#define STALL_WATCH(name, stmt) do { uint64_t sw_ = now_us(); stmt; uint32_t sw_ms = (uint32_t)((now_us() - sw_) / 1000); \
	if (sw_ms > 100) { fprintf(stderr, "stall: %s took %u ms\n", name, sw_ms); PERF.stalls++; } } while (0)

#endif
