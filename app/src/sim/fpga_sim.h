/* Register-level FPGA model + a small machine model (switches, E-stop, pendant).
 *
 * It answers the same register addresses the real FPGA does (hal/fpga_regs.h), so the
 * fpga layer and everything above run unchanged against it. Behaviour is modelled as
 * MEASURED on the unit 2026-10-18 (docs/fpga-motion.md):
 *   - nothing steps unless 0x67 (watchdog) was written within the last WATCHDOG_MS
 *   - position counters 0x02..0x09, latched by each 0x67 write; preset via 0x0C/0x0D + 0x05
 *   - 0x01: bit n = move active, bit 8+n = stepping
 *   - a burst's top rate = 2 x rate word A steps/s; the FPGA's own ramp (word B) is not
 *     modelled (the app sets B to its fastest value and ramps in software)
 *   - same-direction bursts while stepping are appended, opposite ones are IGNORED
 *   - 0x04 = 4 while stepping stops within ~20 ms and leaves the axis HELD: bursts are
 *     ignored until power-off (sim: sim_reset_machine) - the release is still unknown
 * Block mode as MEASURED 2026-11-01 (docs/fpga-motion.md "Block mode"):
 *   - 16 words to 0x50..0x5F, 0x66 = 1 then 0 queues them; 256 deep (a push into a full
 *     queue is dropped and counted in ignored_cmds); read 0x36 = blocks waiting (0x100 when full)
 *   - the head block plays only with the watchdog alive: the dominant channel at 2 x w4
 *     steps/s for w8 steps, the others' steps spread evenly over the same time; the next
 *     block follows in the same ms (no gap, no start overhead); an empty queue just stops
 *   - 0x01 bits 4+ch and 12+ch while a block plays on channel ch (0x2020 measured for Y)
 *   - ASSUMPTIONS: the playing block is not counted in 0x36; the mode write 0x42 is
 *     recorded but not required; 0x20+ch flushes the waiting blocks only when
 *     flush_on_mode_off is set (what the real FPGA does is not measured yet)
 */
#ifndef FPGA_SIM_H
#define FPGA_SIM_H
#include "../common.h"

#define SIM_BURST_MS 10
#define SIM_WATCHDOG_MS 100
#define SIM_BLOCK_QUEUE 256

typedef struct {
	/* machine model, in steps, in the sim's own physical frame */
	int64_t pos[NAXES];          /* true motor position */
	int64_t switch_at[NAXES];    /* shared limit/home switch trips at/beyond this */
	int switch_side[NAXES];      /* -1: trips when pos <= switch_at, +1: when >=, 0: none */
	int64_t hard_lo[NAXES], hard_hi[NAXES];  /* mechanical end: beyond = crash counter */
	uint8_t switch_input[NAXES]; /* INnn wired to that switch (2,3,4 on this machine) */
	bool switch_nc;              /* limit switches normally-closed (vendor level "High") */
	bool wiring_rev[NAXES];      /* motor/driver wired reversed (X on this machine: why X has DIR invert) */
	bool ext_estop;              /* IN01, normally-open push button (vendor level "Low") */
	bool pendant_estop;
	bool pendant_held; int pendant_axis; int pendant_mult;
	uint16_t wheel;
	uint32_t extra_inputs_closed;   /* other INnn forced closed by tests (bit n-1) */
	bool no_watchdog;               /* tests: pretend the heartbeat is always fresh */
	/* observed outputs */
	uint8_t outputs; int pwm; bool beep;
	uint16_t axis_cfg[NAXES];
	uint32_t crashes, ignored_cmds, bursts, heartbeats;
	/* internals */
	uint64_t t_ms, hb_ms;
	uint16_t wr[256];
	uint16_t bank[NAXES][256];
	int sel;
	int32_t left[NAXES];          /* steps still to emit */
	int dir[NAXES];
	double rate[NAXES], acc[NAXES];
	double wait_ms[NAXES];       /* measured ~2 ms start overhead of each burst */
	int64_t cnt[NAXES];           /* FPGA position counter (counts in DIR-bit sense, preset via 0x05) */
	int32_t latched[NAXES];       /* counter value at the last heartbeat */
	bool held[NAXES];
	/* block mode */
	bool flush_on_mode_off;       /* tests: 0x42 = 0x20+ch empties the queue (unmeasured) */
	uint16_t axis_mode[NAXES];    /* last 0x42 written per channel */
	uint32_t blocks, blocks_played;   /* pushed into the queue / finished playing */
	uint16_t blk_stage[16];
	uint16_t blk_q[SIM_BLOCK_QUEUE][16];
	int blk_head, blk_n;          /* waiting blocks (the playing one not included) */
	bool blk_on;                  /* a block is playing */
	int blk_dir[NAXES];           /* counter sense of the playing block per channel */
	uint32_t blk_steps[NAXES], blk_done[NAXES];
	double blk_ms, blk_t;         /* its duration and elapsed time */
} fpga_sim_t;

extern fpga_sim_t SIM;

void sim_reset_machine(void);         /* this garage machine: 2000 steps/mm, switches as wired */
void sim_step(uint32_t ms);           /* advance pulse generation */
uint16_t sim_read(uint8_t addr);
void sim_write(uint8_t addr, uint16_t v);
bool sim_switch_active(int axis);
double sim_pos_mm(int axis);          /* 2000 steps/mm */

#endif
