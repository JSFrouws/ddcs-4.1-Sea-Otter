#include "fpga.h"
#include "fpga_regs.h"

/* Rate words of a burst (measured on the unit 2026-10-18, docs/fpga-motion.md):
 * A (0x12/0x13) = half the top step rate in steps/s; B (0x14/0x15) scales the FPGA's own
 * acceleration ramp. The vendor jogs with A = 0x208D (~16 700 steps/s), B = 0x2710. We set A
 * from the planner's speed and B high, so the planner - not the FPGA - shapes the ramp. */
#define RATE_A 0x208Du
#define RATE_B 0x2710u
#define RATE_B_FAST 0x1000000u   /* measured: 400 steps at 50k/s take 80 ms with B = 0x9C40 (FPGA ramp),
                                  * 8 ms with B = 0x1000000 (no ramp) - our planner does the ramping */

void fpga_attach(fpga_t *f, bus_t *b)
{
	*f = (fpga_t){ .bus = b };
	f->last_mpg = bus_r(b, FR_MPG_COUNT);
}

static void sel_w(fpga_t *f, int ax, uint8_t a, uint16_t v)
{
	bus_w(f->bus, FW_AXIS_SEL, (uint16_t)ax);
	bus_w(f->bus, a, v);
	f->writes += 2;
}

extern const uint16_t FPGA_VENDOR_INIT[][2];
extern const int FPGA_VENDOR_INIT_N;

void fpga_init(fpga_t *f, const fpga_axis_cfg_t cfg[NAXES])
{
	/* 1. the vendor's complete start-up sequence (clock constants, motion parameters 0x10-0x1D,
	 *    input filters, ...), captured on this unit, with three substitutions: our axis config,
	 *    FPGA soft limits opened wide (ours are in software and homing must pass them), and no
	 *    position restore (the FPGA keeps its counters while powered) */
	int sel = 0x0F;
	for (int i = 0; i < FPGA_VENDOR_INIT_N; i++) {
		uint8_t a = (uint8_t)FPGA_VENDOR_INIT[i][0];
		uint16_t v = FPGA_VENDOR_INIT[i][1];
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
	pendant_t p = { .axis = -1, .mult = 1 };
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

void fpga_burst(fpga_t *f, int axis, int32_t steps, uint32_t rate)
{
	if (!steps)
		return;
	uint32_t n = (uint32_t)(steps < 0 ? -steps : steps);
	uint32_t ra = rate ? (rate + 1) / 2 : RATE_A, rb = rate ? RATE_B_FAST : RATE_B;
	if (ra < 16) ra = 16;
	bus_t *b = f->bus;
	bus_w(b, FW_AXIS_SEL, (uint16_t)axis);
	bus_w(b, FW_RATE_A_LO, (uint16_t)(ra & 0xFFFF)); bus_w(b, FW_RATE_A_HI, (uint16_t)(ra >> 16));
	bus_w(b, FW_RATE_B_LO, (uint16_t)(rb & 0xFFFF)); bus_w(b, FW_RATE_B_HI, (uint16_t)(rb >> 16));
	bus_w(b, FW_STEPS_LO, (uint16_t)(n & 0xFFFF));
	bus_w(b, FW_STEPS_HI, (uint16_t)(n >> 16));
	bus_w(b, FW_MOVE_MODE, 2);
	bus_w(b, FW_MOVE_CMD, steps > 0 ? 3 : 1);   /* ASSUMPTION: DIR bit set = positive */
	bus_w(b, FW_MOVE_CMD, 0);
	f->bursts++;
	f->writes += 10;
}

void fpga_status_sweep(fpga_t *f)
{
	bus_w(f->bus, FW_STATUS_ACK, 0);
	for (int ax = 0; ax < NAXES; ax++)
		bus_w(f->bus, FW_AXIS_SEL, (uint16_t)ax);
}

bool fpga_busy(fpga_t *f) { return (bus_r(f->bus, FR_STATUS) & 0x0F00) != 0; }
/* busy = active (bit n) or stepping (bit 8+n): a reversal is only safe when both are clear */
uint8_t fpga_moving(fpga_t *f) { uint16_t s = bus_r(f->bus, FR_STATUS); return (uint8_t)((s | s >> 8) & 0x0F); }
void fpga_heartbeat(fpga_t *f) { bus_w(f->bus, FW_HEARTBEAT, 0); }

uint16_t fpga_status(fpga_t *f) { return bus_r(f->bus, FR_STATUS); }

void fpga_positions(fpga_t *f, int32_t pos[NAXES])
{
	for (int a = 0; a < NAXES; a++) {
		uint32_t hi = bus_r(f->bus, (uint8_t)(FR_POS_LO + 2 * a + 1)), lo = bus_r(f->bus, (uint8_t)(FR_POS_LO + 2 * a));
		pos[a] = (int32_t)(hi << 16 | lo);
	}
}

void fpga_preset(fpga_t *f, int axis, int32_t steps)
{
	bus_w(f->bus, FW_AXIS_SEL, (uint16_t)axis);
	bus_w(f->bus, FW_PRESET_LO, (uint16_t)((uint32_t)steps & 0xFFFF));
	bus_w(f->bus, FW_PRESET_LO + 1, (uint16_t)((uint32_t)steps >> 16));
	bus_w(f->bus, FW_POS_PRESET, 1);
	bus_w(f->bus, FW_POS_PRESET, 0);
	bus_w(f->bus, FW_AXIS_SEL, 0x0F);
}

void fpga_stop(fpga_t *f, int axis)
{
	bus_w(f->bus, FW_AXIS_SEL, (uint16_t)axis);
	bus_w(f->bus, FW_MOVE_CMD, 4);
	bus_w(f->bus, FW_MOVE_CMD, 0);
}
