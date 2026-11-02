#include "fpga.h"
#include "fpga_regs.h"

void fpga_attach(fpga_t *f, bus_t *b)
{
	*f = (fpga_t){ .bus = b };   /* zeroes writes, blocks, block_seq, virt[] */
	for (int a = 0; a < NAXES; a++) f->chan[a] = (int8_t)a;
	f->last_mpg = bus_r(b, FR_MPG_COUNT);
}

static void sel_w(fpga_t *f, int ax, uint8_t a, uint16_t v)
{
	bus_w(f->bus, FW_AXIS_SEL, (uint16_t)ax);
	bus_w(f->bus, a, v);
	f->writes += 2;
}

#include "fpga_vendor_init.h"

void fpga_init(fpga_t *f, const fpga_axis_cfg_t axcfg[NAXES])
{
	/* per CHANNEL: the first axis on it, or a quiet default for an unused connector */
	for (int a = 0; a < NAXES; a++) {
		int o = axcfg[a].out;
		f->chan[a] = (int8_t)(o == 0 ? a : o > 0 && o <= NAXES ? o - 1 : -1);
	}
	fpga_axis_cfg_t cfg[NAXES];
	for (int ch = 0; ch < NAXES; ch++) {
		cfg[ch] = (fpga_axis_cfg_t){ .pulse_high = true };
		for (int a = NAXES - 1; a >= 0; a--) if (f->chan[a] == ch) cfg[ch] = axcfg[a];
	}
	/* 1. the vendor's complete start-up sequence (clock constants, motion parameters 0x10-0x1D,
	 *    input filters, ...), captured on this unit, with three substitutions: our axis config,
	 *    FPGA soft limits opened wide (ours are in software and homing must pass them), and no
	 *    position restore (the FPGA keeps its counters while powered) */
	int sel = 0x0F;
	for (int i = 0; i < FPGA_VENDOR_INIT_N; i++) {
		uint8_t a = FPGA_VENDOR_INIT[i].addr;
		uint16_t v = FPGA_VENDOR_INIT[i].val;
		/* the vendor's own pauses are part of the sequence: 0x44 resets the block-motion
		 * engine and it needs ~50 ms before it takes configuration (2026-11-02, bisected on
		 * the unit: replayed back to back, every block stream ended in the halted FPGA) */
		bus_settle(f->bus, FPGA_VENDOR_INIT[i].wait_us);
		/* ... and so are its READS: 0x40 after every strobe of the serial configuration. With the
		 * writes alone a freshly powered FPGA halts on its first stream; a pause in their place
		 * does not help, the read transaction itself does (2026-11-02, tools/gen_vendor_init.py) */
		if (FPGA_VENDOR_INIT[i].read) { (void)bus_r(f->bus, a); continue; }
		bool banked = sel >= 0 && sel < NAXES;
		if (a == FW_AXIS_SEL) sel = v;
		if (banked && (a == FW_PRESET_LO || a == FW_PRESET_LO + 1 || a == FW_POS_PRESET)) continue;
		if (banked && a >= FW_SOFT_MAX_LO && a <= FW_SOFT_MAX_LO + 3)
			v = (uint16_t[]){ 0xFFFF, 0x7FFF, 0x0000, 0x8000 }[a - FW_SOFT_MAX_LO];
		if (banked && a == FW_AXIS_CFG) v = (cfg[sel].dir_invert ? 2u : 0u) | (cfg[sel].pulse_high ? 4u : 0u);
		bus_w(f->bus, a, v);
		f->writes++;
	}
	/* 2. our own configuration on top (same order the app uses when settings are saved) */
	/* electrical config, as the app writes it on every settings save: sel, 0x02, sel = 0x0F */
	for (int ax = 0; ax < NAXES; ax++) {
		uint16_t v = (cfg[ax].dir_invert ? 2u : 0u) | (cfg[ax].pulse_high ? 4u : 0u);
		sel_w(f, ax, FW_AXIS_CFG, v);
		bus_w(f->bus, FW_AXIS_SEL, 0x0F);
	}
	/* FPGA-internal I/O functions, replaying the app's boot sequence (trace t=4.040-4.056):
	 * every write is preceded by an axis select; 0x01 accumulates enable bits:
	 * bit0 always, bit2 (+limit) or bit10 (none), bit3 (-limit) or bit11, bit12 = no home. */
	for (int ax = 0; ax < NAXES; ax++) {
		const fpga_axis_cfg_t *c = &cfg[ax];
		uint16_t en = 0x0001;
		sel_w(f, ax, FW_ALARM_PORT, (uint16_t)(c->alarm_port << 8));
		sel_w(f, ax, FW_AXIS_ENABLE, en);
		en |= c->limit_port_pos ? 0x0004 : 0x0400;
		sel_w(f, ax, FW_LIMIT_PORTS, c->limit_port_pos);
		sel_w(f, ax, FW_AXIS_ENABLE, en);
		en |= c->limit_port_neg ? 0x0008 : 0x0800;
		sel_w(f, ax, FW_LIMIT_PORTS, (uint16_t)(c->limit_port_neg << 8 | c->limit_port_pos));
		sel_w(f, ax, FW_AXIS_ENABLE, en);
		en |= c->home_port ? 0 : 0x1000;
		sel_w(f, ax, FW_HOME_PORT, c->home_port);
		sel_w(f, ax, FW_AXIS_ENABLE, en);
	}
	f->outputs = 0;
	bus_w(f->bus, FW_OUTPUTS, 0);
}

uint16_t fpga_version(fpga_t *f) { return bus_r(f->bus, FR_VERSION); }

uint32_t fpga_inputs(fpga_t *f)
{
	uint32_t lo = bus_r(f->bus, FR_INPUTS_LO), hi = bus_r(f->bus, FR_INPUTS_HI);
	return (lo & 0xFFFFu) | ((hi & 3u) << 16);
}

pendant_t fpga_pendant(fpga_t *f)
{
	uint16_t v = bus_r(f->bus, FR_PENDANT);
	pendant_t p = { .axis = -1, .mult = 1, .raw = v };
	p.estop = (v & PD_ESTOP) != 0;
	if (!(v & PD_X1)) p.mult = 1, p.held = true;
	else if (!(v & PD_X10)) p.mult = 10, p.held = true;
	else if (!(v & PD_X100)) p.mult = 100, p.held = true;
	if (p.held) {
		if (!(v & PD_AXIS_X)) p.axis = 0;
		else if (!(v & PD_AXIS_Y)) p.axis = 1;
		else if (!(v & PD_AXIS_Z)) p.axis = 2;
		else if (!(v & PD_AXIS_4)) p.axis = 3;
	}
	return p;
}

int16_t fpga_wheel_delta(fpga_t *f)
{
	uint16_t v = bus_r(f->bus, FR_MPG_COUNT);
	int16_t d = (int16_t)(uint16_t)(v - f->last_mpg);
	f->last_mpg = v;
	return d;
}

void fpga_set_outputs(fpga_t *f, uint8_t mask)
{
	mask &= 7;
	if (mask == f->outputs)
		return;
	f->outputs = mask;
	bus_w(f->bus, FW_OUTPUTS, mask);
}

void fpga_spindle_pwm(fpga_t *f, int duty) { bus_pwm(f->bus, CLAMP(duty, 0, 3000)); }

/* a block playing on channel ch sets bits 4+ch and 12+ch (0x2020 measured for channel 1,
 * 2026-11-01). The burst-mode layout (bits ch, 8+ch) is not read since burst mode was
 * removed (2026-11-02): nothing in a block stream was ever seen to set it. */
#define STATUS_BLOCK_BITS 0xF0F0u
bool fpga_busy(fpga_t *f) { return (bus_r(f->bus, FR_STATUS) & STATUS_BLOCK_BITS) != 0; }
/* a preset is only safe when no channel is playing */
uint8_t fpga_moving(fpga_t *f)
{
	uint16_t s = bus_r(f->bus, FR_STATUS);
	uint8_t ch = (uint8_t)((s >> 4 | s >> 12) & 0x0F), ax = 0;
	for (int a = 0; a < NAXES; a++) if (f->chan[a] >= 0 && (ch >> f->chan[a] & 1)) ax |= (uint8_t)(1u << a);
	return ax;
}
void fpga_heartbeat(fpga_t *f) { bus_w(f->bus, FW_HEARTBEAT, 0); }

uint16_t fpga_status(fpga_t *f) { return bus_r(f->bus, FR_STATUS); }

void fpga_positions(fpga_t *f, int32_t pos[NAXES])
{
	for (int a = 0; a < NAXES; a++) {
		int ch = f->chan[a];
		if (ch < 0) { pos[a] = f->virt[a]; continue; }
		uint32_t hi = bus_r(f->bus, (uint8_t)(FR_POS_LO + 2 * ch + 1)), lo = bus_r(f->bus, (uint8_t)(FR_POS_LO + 2 * ch));
		pos[a] = (int32_t)(hi << 16 | lo);
	}
}

void fpga_preset(fpga_t *f, int axis, int32_t steps)
{
	if (f->chan[axis] < 0) { f->virt[axis] = steps; return; }
	axis = f->chan[axis];
	bus_w(f->bus, FW_AXIS_SEL, (uint16_t)axis);
	bus_w(f->bus, FW_PRESET_LO, (uint16_t)((uint32_t)steps & 0xFFFF));
	bus_w(f->bus, FW_PRESET_LO + 1, (uint16_t)((uint32_t)steps >> 16));
	bus_w(f->bus, FW_POS_PRESET, 1);
	bus_w(f->bus, FW_POS_PRESET, 0);
	bus_w(f->bus, FW_AXIS_SEL, 0x0F);
}
