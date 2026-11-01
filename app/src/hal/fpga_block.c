/* Block (continuous-motion) interface of the FPGA - decoded from the vendor app and driven on
 * the unit 2026-11-01 (docs/fpga-motion.md "Block mode", tools/blockmove.c). The word layout
 * is in fpga_regs.h "BLOCK WORDS"; docs/block-mode.md is the design that uses it. */
#include "fpga.h"
#include "fpga_regs.h"

void fpga_block_mode(fpga_t *f, bool on)
{
	for (int ch = 0; ch < NAXES; ch++) {
		bus_w(f->bus, FW_AXIS_SEL, (uint16_t)ch);
		bus_w(f->bus, FW_AXIS_MODE, (uint16_t)((on ? 0x30 : 0x20) + ch));
		bus_w(f->bus, FW_AXIS_SEL, 0x0F);
		f->writes += 3;
	}
}

static uint32_t mag(int32_t v) { return v < 0 ? (uint32_t)-(int64_t)v : (uint32_t)v; }

bool fpga_block(fpga_t *f, const int32_t steps[NAXES], double dt_s)
{
	int32_t c[NAXES] = { 0 };      /* per CHANNEL (two axes on one connector add up) */
	for (int a = 0; a < NAXES; a++) {
		if (f->chan[a] < 0) f->virt[a] += steps[a];   /* no output: count, send nothing */
		else c[f->chan[a]] += steps[a];
	}
	int dom = 0;
	for (int ch = 1; ch < NAXES; ch++) if (mag(c[ch]) > mag(c[dom])) dom = ch;   /* ties: lowest index */
	if (!c[dom])
		return false;
	uint32_t n = mag(c[dom]);
	if (n > 0xFFFF) n = 0xFFFF;    /* a 10 ms block at the 40 000 steps/s ceiling is 400 steps */
	/* rate word = steps/s / 2 (measured); with 10 ms blocks exactly steps x 50 */
	double r = dt_s > 0 ? n / (2.0 * dt_s) + 0.5 : 0xFFFF;
	/* never below the vendor's floor of 200 steps/s (rate word 0x64 is the lowest it ever
	 * sends; its ramps start and end there): a 1-step block at rate word 50 is the one thing
	 * Sea Otter sent that blockmove never did, and the FPGA halted right after such a move
	 * (2026-11-01 evening). Below the floor the block simply plays a little faster. */
	uint16_t w4 = (uint16_t)(r < BLK_W4_MIN ? BLK_W4_MIN : r > 0xFFFF ? 0xFFFF : (uint32_t)r);
	uint16_t w[16] = { 0 };
	w[0] = (uint16_t)(BLK_W0_BASE | (uint16_t)(dom << 1) | (c[dom] > 0 ? BLK_W0_DOM_DIR : 0));
	w[1] = BLK_W1;
	/* word 2: the vendor keeps it constant within a move (0x0c, 0x0d, ... per G-code line);
	 * a running per-block counter stopped the FPGA playing after ~55 blocks on the first
	 * program run (2026-11-01) - whatever it is, it is not a free sequence number */
	w[2] = 0x000c;
	f->block_seq++;
	w[4] = w4;
	w[6] = BLK_W6;
	w[8] = (uint16_t)n;
	/* the remaining channels in index order: directions on bits 8, 7, 6 (1 = +, also for
	 * none - the vendor's single-axis blocks set them), steps in w10, w12, w14 */
	for (int ch = 0, k = 0; ch < NAXES; ch++) {
		if (ch == dom) continue;
		uint32_t m = mag(c[ch]);
		if (c[ch] >= 0) w[0] |= (uint16_t)(1u << (8 - k));
		w[10 + 2 * k] = (uint16_t)(m > 0xFFFF ? 0xFFFF : m);
		k++;
	}
	bus_block(f->bus, w);
	f->blocks++;
	f->writes += 18;
	return true;
}

/* blocks queued. A FULL queue reads 0x0100 (256), not 0xff - masking it to 0 made the app
 * push 5 blocks a tick into a halted FPGA (2026-11-01); the block ioctl then hangs the
 * process. Never push at FPGA_QUEUE_FULL or above. */
int fpga_block_queue(fpga_t *f) { return bus_r(f->bus, FR_BLOCK_QUEUE) & 0x1FF; }

/* The only candidate we have for emptying the queue (an immediate stop): the vendor's
 * end-of-stream mode write, then back into block mode. TO BE MEASURED on the unit - whether
 * 0x20+ch flushes the queue is unknown; worst case the queued blocks play out. */
void fpga_block_flush(fpga_t *f)
{
	fpga_block_mode(f, false);
	fpga_block_mode(f, true);
}
