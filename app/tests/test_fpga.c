/* FPGA layer against its counterparts: (1) the register sequences the vendor app was
 * recorded writing on the real unit, (2) the register-level simulator. */
#include <stdio.h>
#include <string.h>
#include "t.h"
#include "hal/fpga.h"
#include "sim/fpga_sim.h"

#define TRACE "../../docs/raw/2026-10-16_fpga-outputs/iotrace_run3.log"

typedef struct { uint8_t a; uint16_t v; } wr_t;
static wr_t got[4096]; static int ngot;
static void rec(void *ctx, char d, uint8_t a, uint16_t v) { if (d == 'W' && ngot < 4096) got[ngot++] = (wr_t){ a, v }; }

/* all WRITE lines of the vendor trace */
static wr_t vt[20000]; static int nvt;
static int load_trace(void)
{
	FILE *f = fopen(TRACE, "r");
	if (!f) return -1;
	char line[160];
	unsigned a, v;
	while (fgets(line, sizeof line, f) && nvt < 20000) {
		char *p = strstr(line, "WRITE 0x");
		if (p && sscanf(p, "WRITE 0x%x = 0x%x", &a, &v) == 2) vt[nvt++] = (wr_t){ (uint8_t)a, (uint16_t)v };
	}
	fclose(f);
	return 0;
}
static int find(uint8_t a, uint16_t v, int from) { for (int i = from; i < nvt; i++) if (vt[i].a == a && vt[i].v == v) return i; return -1; }

static bool same(const wr_t *x, const wr_t *y, int n, const char *what)
{
	for (int i = 0; i < n; i++) if (x[i].a != y[i].a || x[i].v != y[i].v) {
		printf("  %s differs at write %d: ours 0x%02x=0x%04x, vendor 0x%02x=0x%04x\n", what, i, x[i].a, x[i].v, y[i].a, y[i].v);
		return false;
	}
	return true;
}

void test_fpga(void)
{
	SUITE("fpga layer vs vendor trace and simulator");
	sim_reset_machine();
	bus_t *b = bus_sim_open();
	b->trace = rec;
	fpga_t f;
	fpga_attach(&f, b);
	bool have = load_trace() == 0;
	if (!have) printf("  (vendor trace not found at %s - trace comparisons skipped)\n", TRACE);

	/* 1. boot-time I/O function setup, this machine's ports (X/Y/Z switch IN02-04, alarms IN10-13) */
	fpga_axis_cfg_t cfg[NAXES] = {
		{ 2, 2, 2, 10, true, true }, { 3, 3, 3, 11, false, true }, { 4, 4, 4, 12, false, true }, { 0, 0, 0, 13, false, true } };
	ngot = 0;
	fpga_init(&f, cfg);
	int start = 0;
	while (start < ngot && got[start].a != 0x36) start++;
	CHECK(start > 0 && start < ngot);
	if (have) {
		int i = find(0x36, 0x0a00, 0);
		CHECK(i > 0);
		if (i > 0) CHECK(same(&got[start - 1], &vt[i - 1], 64, "I/O init"));
	}
	CHECK(SIM.axis_cfg[0] == 6 && SIM.axis_cfg[1] == 4);   /* X: DIR invert + pulse high, as the unit's config */

	/* 2. a 50-step burst = the app's first MPG jog burst, write for write */
	ngot = 0;
	fpga_burst(&f, 0, 50, 0);
	CHECK(ngot == 10);
	if (have) {
		int i = find(0x0a, 0x0032, 0);
		CHECK(i >= 6);
		if (i >= 6) CHECK(same(got, &vt[i - 5], 10, "jog burst"));
	}
	/* 3. the status sweep after a burst */
	ngot = 0;
	fpga_status_sweep(&f);
	wr_t sweep[5] = { { 0x2e, 0 }, { 0, 0 }, { 0, 1 }, { 0, 2 }, { 0, 3 } };
	CHECK(ngot == 5 && same(got, sweep, 5, "status sweep"));

	/* 4. the simulator executes it: +50 steps on X (DIR invert + reversed wiring = correct way) */
	int64_t x0 = SIM.pos[0];
	sim_step(SIM_BURST_MS);
	CHECK(SIM.pos[0] == x0);                   /* measured: no heartbeat, no steps */
	fpga_heartbeat(&f);
	sim_step(SIM_BURST_MS);
	CHECK(SIM.pos[0] - x0 == 50);
	fpga_burst(&f, 1, -400, 40000);          /* 40 000 steps/s = 400 steps per 10 ms */
	CHECK(fpga_busy(&f) && (fpga_status(&f) & 0x0202) == 0x0202);
	fpga_heartbeat(&f);
	sim_step(SIM_BURST_MS + 3);           /* + the ~2 ms start overhead */
	CHECK(!fpga_busy(&f));
	int32_t cnt[NAXES];
	fpga_heartbeat(&f);                       /* latches the counters */
	fpga_positions(&f, cnt);
	CHECK(cnt[0] == (int32_t)(464.207 * 2000) + 50 && cnt[1] == (int32_t)(274.491 * 2000) - 400);
	fpga_preset(&f, 2, 12345);
	fpga_heartbeat(&f); fpga_positions(&f, cnt);
	CHECK(cnt[2] == 12345);

	/* 5. inputs and pendant decoding */
	CHECK((fpga_inputs(&f) & 1) == 1);          /* IN01 open: E-stop button not pressed */
	SIM.ext_estop = true;
	CHECK((fpga_inputs(&f) & 1) == 0);
	SIM.ext_estop = false;
	pendant_t p = fpga_pendant(&f);
	CHECK(!p.held && p.axis == -1 && !p.estop);
	SIM.pendant_held = true; SIM.pendant_axis = 2; SIM.pendant_mult = 10;
	p = fpga_pendant(&f);
	CHECK(p.held && p.axis == 2 && p.mult == 10);
	SIM.pendant_axis = -1;                       /* OFF: dead-man held, no axis bit */
	p = fpga_pendant(&f);
	CHECK(p.held && p.axis == -1);
	SIM.pendant_estop = true;
	CHECK(fpga_pendant(&f).estop);
	SIM.pendant_estop = false;
	SIM.wheel = 0xFFFE; fpga_wheel_delta(&f);
	SIM.wheel = 0x0002;
	CHECK(fpga_wheel_delta(&f) == 4);           /* wraps cleanly */

	/* 6. outputs */
	fpga_set_outputs(&f, 5);
	CHECK(SIM.outputs == 5);

	/* 7. measured on the unit: same-direction bursts append, a reversal while stepping is
	 *    ignored, and a STOP mid-move holds the axis (further bursts ignored) */
	fpga_heartbeat(&f);
	int64_t a0 = SIM.cnt[3];
	fpga_burst(&f, 3, 300, 10000); fpga_burst(&f, 3, 300, 10000);   /* appended */
	fpga_burst(&f, 3, -500, 10000);                                  /* ignored */
	for (int i = 0; i < 10; i++) { fpga_heartbeat(&f); sim_step(SIM_BURST_MS); }
	CHECK(SIM.cnt[3] - a0 == 600 && SIM.ignored_cmds == 1);
	fpga_burst(&f, 3, 10000, 10000);
	fpga_heartbeat(&f); sim_step(SIM_BURST_MS);
	fpga_stop(&f, 3);
	for (int i = 0; i < 20; i++) { fpga_heartbeat(&f); sim_step(SIM_BURST_MS); }
	CHECK(SIM.held[3] && (fpga_status(&f) & 0x0808) == 0x0008);
	int64_t a1 = SIM.cnt[3];
	fpga_burst(&f, 3, 100, 10000); fpga_heartbeat(&f); sim_step(SIM_BURST_MS);
	CHECK(SIM.cnt[3] == a1);
	b->close(b);
}
