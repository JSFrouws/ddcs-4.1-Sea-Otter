/* Shared basics for the DDCS V4.1 replacement app. */
#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define NAXES 4                 /* X Y Z A - the FPGA has four pulse/dir channels */
enum { AX_X, AX_Y, AX_Z, AX_A };
extern const char AXIS_CH[NAXES];   /* "XYZA" */

#define TICK_MS 10              /* main loop / motion slice period */

/* monotonic milliseconds (real clock, or the simulator's clock in tests) */
uint64_t now_ms(void);
void clock_use_virtual(bool on);     /* tests: time only moves via clock_advance() */
void clock_advance(uint32_t ms);

#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#endif
