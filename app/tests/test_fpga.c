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

/* one 1 ms tick with the watchdog written, as the main loop does it */
static void tick1(fpga_t *f) { fpga_heartbeat(f); sim_step(1); }
static uint16_t gw(int i) { return got[i].v; }   /* block word i of the last block traced */

/* Block mode (docs/block-mode.md): the word encoding against the vendor trace / today's
 * measurements, and the sim's queue model. */
static void test_fpga_blocks(void)
{
	SUITE("fpga block mode: encoding and queue model");
	sim_reset_machine();
	bus_t *b = bus_sim_open();
	b->trace = rec;
	fpga_t f;
	fpga_attach(&f, b);
	CHECK(f.blocks == 0 && f.block_seq == 0);

	/* mode writes, per channel, as the vendor (and blockmove) */
	ngot = 0;
	fpga_block_mode(&f, true);
	CHECK(ngot == 12 && got[0].a == 0x00 && got[0].v == 0 && got[1].a == 0x42 && got[1].v == 0x30 && got[2].a == 0x00 && got[2].v == 0x0f);
	CHECK(got[10].a == 0x42 && got[10].v == 0x33);
	CHECK(SIM.axis_mode[0] == 0x30 && SIM.axis_mode[3] == 0x33);

	/* (a) Y only, +20 steps in 10 ms: 03e3 (bits 9..6 all set), rate word 1000 = 2000 steps/s / 2 */
	ngot = 0;
	CHECK(fpga_block(&f, (int32_t[NAXES]){ 0, 20, 0, 0 }, 0.010));
	CHECK(ngot == 18);
	bool seq = ngot == 18;
	for (int i = 0; i < 16 && seq; i++) seq = got[i].a == 0x50 + i;
	CHECK(seq && got[16].a == 0x66 && got[16].v == 1 && got[17].a == 0x66 && got[17].v == 0);
	CHECK(gw(0) == 0x03e3 && gw(1) == 0x3020 && gw(2) == 0 && gw(4) == 1000 && gw(6) == 0x0d05 && gw(8) == 20);
	CHECK(gw(10) == 0 && gw(12) == 0 && gw(14) == 0 && gw(3) == 0 && gw(5) == 0 && gw(15) == 0);
	CHECK(f.blocks == 1 && f.block_seq == 1 && SIM.blocks == 1 && fpga_block_queue(&f) == 1);
	CHECK(!fpga_block(&f, (int32_t[NAXES]){ 0 }, 0.010) && f.blocks == 1);   /* nothing to move: nothing sent */

	/* (b) X-Y- = the vendor's 00e1: X dominant on a tie, Y's direction on bit 8 */
	ngot = 0;
	fpga_block(&f, (int32_t[NAXES]){ -20, -20, 0, 0 }, 0.010);
	CHECK(gw(0) == 0x00e1 && gw(2) == 1 && gw(8) == 20 && gw(10) == 20 && gw(12) == 0);
	ngot = 0;
	fpga_block(&f, (int32_t[NAXES]){ -19, -20, 0, 0 }, 0.010);   /* |Y| > |X|: Y dominant, X first remaining */
	CHECK(gw(0) == 0x00e3 && gw(8) == 20 && gw(10) == 19);

	/* (c) Y dominant with Z: Z in w12, its direction on bit 7 (measured 2026-11-01) */
	ngot = 0;
	fpga_block(&f, (int32_t[NAXES]){ 0, 40, -20, 0 }, 0.010);
	CHECK(gw(0) == (0x03e3 & ~0x0080) && gw(4) == 2000 && gw(8) == 40 && gw(10) == 0 && gw(12) == 20 && gw(14) == 0);

	/* (f) queued, but no watchdog: nothing plays */
	CHECK(fpga_block_queue(&f) == 4);
	int64_t c0[NAXES]; memcpy(c0, SIM.cnt, sizeof c0);
	sim_step(200);
	CHECK(!memcmp(c0, SIM.cnt, sizeof c0) && fpga_block_queue(&f) == 4 && SIM.blocks_played == 0);
	/* the four play out once the heartbeat runs: Y +20 -20 -20 +40, X -20 -19, Z -20 */
	for (int i = 0; i < 60; i++) tick1(&f);
	CHECK(SIM.cnt[1] - c0[1] == 20 && SIM.cnt[0] - c0[0] == -39 && SIM.cnt[2] - c0[2] == -20);
	CHECK(fpga_block_queue(&f) == 0 && SIM.blocks_played == 4 && !fpga_moving(&f) && !fpga_busy(&f));
	/* a full queue: 256 deep, reads 0xff, the 257th is dropped, still nothing without heartbeat */
	sim_reset_machine();
	for (int i = 0; i < SIM_BLOCK_QUEUE + 1; i++) fpga_block(&f, (int32_t[NAXES]){ 0, 1, 0, 0 }, 0.010);
	memcpy(c0, SIM.cnt, sizeof c0);
	sim_step(200);
	CHECK(fpga_block_queue(&f) == 0xff && SIM.ignored_cmds == 1 && SIM.cnt[1] == c0[1]);

	/* (d) 10 blocks of +20 Y at 4000 steps/s: 4 steps every ms, no gap between blocks */
	sim_reset_machine();
	ngot = 0;
	for (int i = 0; i < 10; i++) fpga_block(&f, (int32_t[NAXES]){ 0, 20, 0, 0 }, 20 / 4000.0);
	CHECK(fpga_block_queue(&f) == 10 && ngot == 180 && got[ngot - 18 + 4].v == 2000);
	memcpy(c0, SIM.cnt, sizeof c0);
	int64_t pos0 = SIM.pos[1];
	bool steady = true, status_ok = true;
	for (int ms = 0; ms < 50; ms++) {
		int64_t before = SIM.cnt[1];
		tick1(&f);
		if (SIM.cnt[1] - before != 4) steady = false;
		if (ms < 49 && ((fpga_status(&f) & 0x2020) != 0x2020 || fpga_moving(&f) != 2 || !fpga_busy(&f))) status_ok = false;
	}
	CHECK(steady && status_ok);
	CHECK(SIM.cnt[1] - c0[1] == 200 && SIM.pos[1] - pos0 == 200 && fpga_block_queue(&f) == 0);
	CHECK(SIM.blocks_played == 10 && fpga_status(&f) == 0 && !fpga_moving(&f));
	for (int i = 0; i < 20; i++) tick1(&f);   /* empty queue: the axis simply stops */
	CHECK(SIM.cnt[1] - c0[1] == 200);

	/* (e) a reversal between consecutive blocks lands exactly */
	memcpy(c0, SIM.cnt, sizeof c0);
	fpga_block(&f, (int32_t[NAXES]){ 0, 30, 0, 0 }, 0.010);
	fpga_block(&f, (int32_t[NAXES]){ 0, -10, 0, 0 }, 0.010);
	for (int i = 0; i < 25; i++) tick1(&f);
	CHECK(SIM.cnt[1] - c0[1] == 20 && SIM.crashes == 0);

	/* (g) two axes: Y 40 dominant, Z 20 spread evenly over the same 10 ms */
	memcpy(c0, SIM.cnt, sizeof c0);
	fpga_block(&f, (int32_t[NAXES]){ 0, 40, 20, 0 }, 0.010);
	for (int i = 0; i < 5; i++) tick1(&f);
	CHECK(SIM.cnt[1] - c0[1] == 20 && SIM.cnt[2] - c0[2] == 10 && (fpga_status(&f) & 0x6060) == 0x6060);
	for (int i = 0; i < 10; i++) tick1(&f);
	CHECK(SIM.cnt[1] - c0[1] == 40 && SIM.cnt[2] - c0[2] == 20 && SIM.cnt[0] == c0[0] && SIM.cnt[3] == c0[3]);
	/* X: DIR invert + reversed wiring = the counter's direction physically, as for bursts */
	SIM.axis_cfg[0] = 2;
	int64_t px = SIM.pos[0]; c0[0] = SIM.cnt[0];
	fpga_block(&f, (int32_t[NAXES]){ 15, 0, 0, 0 }, 0.010);
	for (int i = 0; i < 12; i++) tick1(&f);
	CHECK(SIM.cnt[0] - c0[0] == 15 && SIM.pos[0] - px == 15);

	/* (h) an axis without a channel counts in virt[] and is not in the block */
	f.chan[2] = -1;
	ngot = 0;
	CHECK(fpga_block(&f, (int32_t[NAXES]){ 0, 10, 7, 0 }, 0.010));
	CHECK(f.virt[2] == 7 && gw(0) == 0x03e3 && gw(8) == 10 && gw(10) == 0 && gw(12) == 0 && gw(14) == 0);
	CHECK(!fpga_block(&f, (int32_t[NAXES]){ 0, 0, 5, 0 }, 0.010) && f.virt[2] == 12);
	int32_t pos[NAXES];
	fpga_heartbeat(&f); fpga_positions(&f, pos);
	CHECK(pos[2] == 12);
	f.chan[2] = 2;
	for (int i = 0; i < 12; i++) tick1(&f);

	/* flush = mode off/on: empties the queue only if the FPGA does (unmeasured: a sim flag) */
	for (int i = 0; i < 3; i++) fpga_block(&f, (int32_t[NAXES]){ 0, 10, 0, 0 }, 0.010);
	fpga_block_flush(&f);
	CHECK(fpga_block_queue(&f) == 3 && SIM.axis_mode[1] == 0x31);
	SIM.flush_on_mode_off = true;
	fpga_block_flush(&f);
	CHECK(fpga_block_queue(&f) == 0);
	b->close(b);
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
	test_fpga_blocks();
}
