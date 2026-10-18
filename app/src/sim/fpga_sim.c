#include <string.h>
#include "fpga_sim.h"
#include "../hal/fpga_regs.h"

fpga_sim_t SIM;

#define SPM 2000
#define MAX_RATE_PER_MS 40.0       /* measured: ~40 000 steps/s ceiling on the real FPGA */

void sim_reset_machine(void)
{
	memset(&SIM, 0, sizeof SIM);
	/* physical frame: X/Y switches at the negative end (0), Z switch at the top (0).
	 * Power-up position = where the photo of the real screen showed the machine. */
	const double start[NAXES] = { 464.207, 274.491, -91.201, -1.0 };
	const double lo[NAXES] = { -3, -3, -305, -1e9 }, hi[NAXES] = { 930, 605, 3, 1e9 };
	for (int a = 0; a < NAXES; a++) {
		SIM.pos[a] = (int64_t)(start[a] * SPM);
		SIM.cnt[a] = SIM.latched[a] = (int32_t)SIM.pos[a];   /* the vendor app restored these */
		SIM.hard_lo[a] = (int64_t)(lo[a] * SPM);
		SIM.hard_hi[a] = (int64_t)(hi[a] * SPM);
		SIM.switch_at[a] = 0;
	}
	SIM.switch_side[AX_X] = -1; SIM.switch_side[AX_Y] = -1; SIM.switch_side[AX_Z] = +1;
	SIM.switch_side[AX_A] = 0;
	SIM.switch_input[AX_X] = 2; SIM.switch_input[AX_Y] = 3; SIM.switch_input[AX_Z] = 4;
	SIM.switch_nc = true;
	SIM.wiring_rev[AX_X] = true;
	SIM.pendant_axis = -1; SIM.pendant_mult = 1;
	SIM.sel = 0x0F;
}

bool sim_switch_active(int a)
{
	if (SIM.switch_side[a] < 0) return SIM.pos[a] <= SIM.switch_at[a];
	if (SIM.switch_side[a] > 0) return SIM.pos[a] >= SIM.switch_at[a];
	return false;
}

double sim_pos_mm(int a) { return (double)SIM.pos[a] / SPM; }

/* electrical level of INn: 1 = open, 0 = closed to COM- */
static uint32_t input_levels(void)
{
	uint32_t closed = SIM.extra_inputs_closed;
	if (SIM.ext_estop) closed |= 1u << 0;                 /* IN01, NO button */
	for (int a = 0; a < NAXES; a++) {
		int in = SIM.switch_input[a];
		if (!in) continue;
		bool tripped = sim_switch_active(a);
		bool is_closed = SIM.switch_nc ? !tripped : tripped;
		if (is_closed) closed |= 1u << (in - 1);
	}
	return ~closed & 0x3FFFFu;
}

uint16_t sim_read(uint8_t a)
{
	if (a >= FR_POS_LO && a < FR_POS_LO + 2 * NAXES) {
		uint32_t v = (uint32_t)SIM.latched[(a - FR_POS_LO) / 2];
		return (uint16_t)(((a - FR_POS_LO) & 1) ? v >> 16 : v & 0xFFFF);
	}
	switch (a) {
	case FR_SELECTED: return (uint16_t)SIM.sel;
	case FR_STATUS: {
		uint16_t s = 0;
		for (int i = 0; i < NAXES; i++) {
			if (SIM.left[i] || SIM.held[i]) s |= (uint16_t)(1u << i);
			if (SIM.left[i]) s |= (uint16_t)(1u << (8 + i));
		}
		return s;
	}
	case FR_MPG_COUNT: return SIM.wheel;
	case FR_ALARM: return (uint16_t)((SIM.ext_estop ? 1 : 0) | (SIM.pendant_estop ? 2 : 0));
	case FR_INPUTS_LO: return (uint16_t)(input_levels() & 0xFFFF);
	case FR_INPUTS_HI: return (uint16_t)(0x1FC | ((input_levels() >> 16) & 3));
	case FR_KEYS_EXT: return (uint16_t)((~input_levels() >> 14) & 0xF);
	case FR_PENDANT: {
		uint16_t v = 0x7F;
		if (SIM.pendant_held) {
			v &= (uint16_t)~(SIM.pendant_mult == 100 ? PD_X100 : SIM.pendant_mult == 10 ? PD_X10 : PD_X1);
			if (SIM.pendant_axis >= 0) v &= (uint16_t)~(PD_AXIS_X << SIM.pendant_axis);
		}
		if (SIM.pendant_estop) v |= PD_ESTOP;
		return v;
	}
	case FR_VERSION: return 0x0809;
	default:
		if (a >= FR_AXIS_IOSTAT && a < FR_AXIS_IOSTAT + NAXES)
			return sim_switch_active(a - FR_AXIS_IOSTAT) ? 0x0030 : 0;
		return 0xFFFF;
	}
}

static void start_burst(int ax)
{
	uint16_t *bk = SIM.bank[ax];
	int32_t n = (int32_t)((uint32_t)bk[FW_STEPS_HI] << 16 | bk[FW_STEPS_LO]);
	int dir = (SIM.wr[FW_MOVE_CMD] & 2) ? +1 : -1;           /* counter sense */
	if (SIM.held[ax] || (SIM.left[ax] && SIM.dir[ax] != dir)) { SIM.ignored_cmds++; return; }
	SIM.dir[ax] = dir;
	if (!SIM.left[ax]) SIM.wait_ms[ax] = 2;   /* measured: ~2 ms before a burst starts stepping */
	SIM.left[ax] += n;
	uint32_t ra = (uint32_t)bk[FW_RATE_A_HI] << 16 | bk[FW_RATE_A_LO];
	double r = ra ? 2.0 * ra / 1000.0 : MAX_RATE_PER_MS;     /* measured: top rate = 2 x A steps/s */
	SIM.rate[ax] = r > MAX_RATE_PER_MS ? MAX_RATE_PER_MS : r;
	SIM.bursts++;
}

void sim_write(uint8_t a, uint16_t v)
{
	uint16_t prev = SIM.wr[a];
	SIM.wr[a] = v;
	if (a == FW_AXIS_SEL) { SIM.sel = v; return; }
	if (a == FW_HEARTBEAT) {
		SIM.hb_ms = SIM.t_ms; SIM.heartbeats++;
		for (int i = 0; i < NAXES; i++) SIM.latched[i] = (int32_t)SIM.cnt[i];
		return;
	}
	bool banked = SIM.sel >= 0 && SIM.sel < NAXES;
	if (banked) {
		SIM.bank[SIM.sel][a] = v;
		if (a == FW_AXIS_CFG) SIM.axis_cfg[SIM.sel] = v;
		if (a == FW_POS_PRESET && (v & 1) && !(prev & 1))
			SIM.cnt[SIM.sel] = (int32_t)((uint32_t)SIM.bank[SIM.sel][FW_PRESET_LO + 1] << 16 | SIM.bank[SIM.sel][FW_PRESET_LO]);
		if (a == FW_MOVE_CMD && (v & 1) && !(prev & 1)) start_burst(SIM.sel);
		if (a == FW_MOVE_CMD && (v & 4) && !(prev & 4) && SIM.left[SIM.sel]) {   /* stop -> held */
			int32_t decel = (int32_t)(SIM.rate[SIM.sel] * 10);
			if (SIM.left[SIM.sel] > decel) SIM.left[SIM.sel] = decel;
			SIM.held[SIM.sel] = true;
		}
	}
	if (a == FW_OUTPUTS) SIM.outputs = (uint8_t)(v & 7);
}

void sim_step(uint32_t ms)
{
	SIM.t_ms += ms;
	bool alive = SIM.no_watchdog || (SIM.heartbeats && SIM.t_ms - SIM.hb_ms <= SIM_WATCHDOG_MS);
	for (int a = 0; a < NAXES; a++) {
		if (!SIM.left[a] || !alive) continue;
		double run = ms;
		if (SIM.wait_ms[a] > 0) { double w = SIM.wait_ms[a] < run ? SIM.wait_ms[a] : run; SIM.wait_ms[a] -= w; run -= w; }
		SIM.acc[a] += SIM.rate[a] * run;
		int32_t n = (int32_t)SIM.acc[a];
		if (n > SIM.left[a]) n = SIM.left[a];
		SIM.acc[a] -= n;
		SIM.left[a] -= n;
		SIM.cnt[a] += (int64_t)SIM.dir[a] * n;
		int phys = SIM.dir[a];
		if (SIM.axis_cfg[a] & 2) phys = -phys;      /* DIR invert flips the DIR line ... */
		if (SIM.wiring_rev[a]) phys = -phys;        /* ... and reversed wiring flips it back */
		SIM.pos[a] += (int64_t)phys * n;
		if (SIM.pos[a] < SIM.hard_lo[a]) { SIM.pos[a] = SIM.hard_lo[a]; SIM.crashes++; }
		if (SIM.pos[a] > SIM.hard_hi[a]) { SIM.pos[a] = SIM.hard_hi[a]; SIM.crashes++; }
		if (!SIM.left[a]) SIM.acc[a] = 0;
	}
}
