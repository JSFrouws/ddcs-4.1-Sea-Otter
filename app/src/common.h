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
/* Directory listing that works on the controller: the C library's opendir() fails there
 * with EINVAL (static glibc vs the 2.6.35 kernel - measured), plain open() + getdents64 work.
 * Calls fn(name, ctx) for each entry except "." and ".."; returns the count, -1 on error. */
/* Replace a file safely: does nothing if it already holds exactly these bytes (no flash wear),
 * else writes <path>.new, flushes it to flash and renames it over - a power cut leaves the old
 * file or the new one, never an empty or half-written one. 0 ok / unchanged, -1 error. */
int file_replace(const char *path, const char *data, size_t len);
int dir_list(const char *dir, void (*fn)(const char *name, void *ctx), void *ctx);
void clock_use_virtual(bool on);     /* tests: time only moves via clock_advance() */
void clock_advance(uint32_t ms);

#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#endif
