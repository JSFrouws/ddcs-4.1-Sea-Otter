/* Minimal test harness: CHECK records failures and keeps going. */
#ifndef T_H
#define T_H
#include <stdio.h>
#include <math.h>

extern int t_fail, t_pass;
#define CHECK(c) do { if (c) t_pass++; else { t_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_NEAR(a, b, tol) do { double _a = (a), _b = (b); if (fabs(_a - _b) <= (tol)) t_pass++; else { t_fail++; \
	printf("  FAIL %s:%d  %s = %.6f, expected %.6f\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)
#define SUITE(name) printf("%s\n", name)

/* shared fixture: fresh sim + controller, virtual clock */
void fx_reset(void);
void fx_run(int ms);                 /* advance controller + sim in 10 ms ticks */
void fx_until_idle(int max_ms);
void fx_restart(void);
/* the FPGA rate the planner may sustain: the full 40 000 steps/s in block mode, 80 % with bursts (their ~2 ms start) */
#define CAP_SPS (SET.block_mode ? (double)FPGA_MAX_SPS : FPGA_MAX_SPS * 0.8)               /* ctl_init again on the same files (app restart) */

#endif
