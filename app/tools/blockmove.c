/*
 * blockmove.c -- EXPERIMENT (2026-11-01): drive one axis through the FPGA's block (continuous)
 * interface, replaying what the vendor app was traced doing for a G1 move
 * (docs/raw/2026-11-01_vendor-blocks.log): per-axis mode writes, a start block, then one
 * 16-word block per 5 ms kept in a 256-deep FIFO (read 0x36 = fill, 0xff = full), the
 * 20 MHz constant re-written to 0x23/0x24 every 100 ms, position read from 0x02+2*axis.
 * Trapezoid ramp with accel A. Nothing else may hold /dev/motion (stop seaotter and the
 * vendor app first).
 *
 * Build: arm-linux-gnueabi-gcc -static -O2 -Wall -o blockmove blockmove.c
 * Run:   ./blockmove <axis 0-3> <steps, signed> <steps/s> [accel steps/s^2] [dry]
 */
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#define IOC_WRITE 0x4620u
#define IOC_READ  0x4621u
#define IOC_BLOCK 0x4622u

static int fd, dry;
static double t0;
static double now(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }
static void W(unsigned a, unsigned v)
{
	uint32_t io[2] = { a, v };
	if (dry) { printf("%8.4f W %02x=%04x\n", now() - t0, a, v); return; }
	if (ioctl(fd, IOC_WRITE, io) < 0) perror("write");
}
static unsigned R(unsigned a)
{
	uint32_t io[2] = { a, 0 };
	if (dry) return 0x80;
	if (ioctl(fd, IOC_READ, io) < 0) perror("read");
	return io[1] & 0xFFFF;
}
static void BLOCK(const uint16_t *w)
{
	uint16_t b[18];
	memcpy(b, w, 32); b[16] = 0; b[17] = 0;
	if (dry || getenv("VERBOSE")) {
		printf("%8.4f BLOCK", now() - t0);
		for (int i = 0; i < 16; i++) printf(" %04x", b[i]);
		printf("\n");
	}
	if (!dry && ioctl(fd, IOC_BLOCK, b) < 0) perror("block");
}
static void mode_all(unsigned m)                       /* W 00=a; W 42=m+a; W 00=0f  for every axis */
{
	for (unsigned a = 0; a < 4; a++) { W(0x00, a); W(0x42, m + a); W(0x00, 0x0f); }
}
static void select_all(void) { W(0x2e, 0); for (unsigned a = 0; a < 4; a++) W(0x00, a); }
static void clock_const(void) { if (getenv("NOCLOCK")) return; W(0x23, 0x2d00); W(0x24, 0x0131); }   /* 0x01312d00 = 20 000 000 */
static int32_t pos(int axis) { unsigned lo = R(0x02 + 2 * axis), hi = R(0x03 + 2 * axis); return (int32_t)((hi << 16) | lo); }

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: %s axis steps sps [accel] [fifo depth] [dry]\n", argv[0]); return 2; }
	int axis = atoi(argv[1]); long steps = atol(argv[2]); double vmax = atof(argv[3]);
	double A = argc > 4 ? atof(argv[4]) : 20000;
	unsigned depth = argc > 5 ? (unsigned)atoi(argv[5]) : 0xff;   /* keep at most this many blocks queued (0xff = the FIFO's own limit) */
	const char *pattern = argc > 6 ? argv[6] : "";          /* "zz": reverse every 100 steps in ONE stream; "gap": stop feeding 300 ms half way */
	dry = argc > 7;
	unsigned w6 = getenv("W6") ? (unsigned)strtoul(getenv("W6"), NULL, 0) : 0x0d05;   /* block duration? 3333 x 1.5 us = 5 ms */
	int ax2 = getenv("AX2") ? atoi(getenv("AX2")) : -1;   /* second axis: same steps per block */
	int ax2w = getenv("AX2W") ? atoi(getenv("AX2W")) : 10; if (ax2w < 9 || ax2w > 15) ax2w = 10;
	int ax2dir = getenv("AX2DIR") ? atoi(getenv("AX2DIR")) : (steps < 0 ? 0 : 1);   /* bit 8 = the second axis' direction (traced: X-Y- = 00e1) */
	unsigned axflag = getenv("AXFLAG") ? (unsigned)strtoul(getenv("AXFLAG"), NULL, 0) : (axis == 1 ? 0x02 : axis == 2 ? 0x04 : axis == 3 ? 0x06 : 0);
	if (!dry) { fd = open("/dev/motion", O_RDWR); if (fd < 0) { perror("/dev/motion"); return 1; } }
	t0 = now();
	int dir = steps < 0 ? 0 : 1; long left = labs(steps);
	int32_t p0 = pos(axis), p2 = ax2 >= 0 ? pos(ax2) : 0;
	double t_first = 0, t_last = 0;
	printf("axis %d: %ld steps at %.0f steps/s, accel %.0f, position before %d\n", axis, steps, vmax, A, p0);

	/* --- the vendor's program start --- */
	W(0x0c, 0x0190); W(0x0d, 0); W(0x32, 1); W(0x32, 0);   /* as traced (0x190 = 400 = steps/mm?) */
	mode_all(0x30);
	static uint16_t start_blk[16] = { 0x0400, 0x6f62, 0x000b, 0, 0, 0, 0x001c, 0, 0x001c, 0, 0x001c, 0, 0, 0, 0xffff, 0xffff };
	if (!getenv("NOCTRL")) BLOCK(start_blk);
	select_all();
	mode_all(0x20);
	usleep(20000);
	mode_all(0x30);
	usleep(10000);

	/* --- the move: 5 ms blocks, trapezoid, never slower than 200 steps/s (the vendor's floor) --- */
	uint16_t seq = 0x000c;
	double v = 0, dt = (getenv("BLK_MS") ? atof(getenv("BLK_MS")) : 5) / 1000.0, hb = now();   /* block length: steps = v x dt, rate word v/2 */
	long sent = 0; int blocks = 0, waits = 0;
	uint16_t w0 = (uint16_t)(0x03e1 | axflag);         /* bit1 = Y, bit2 = Z dominant (traced); A: AXFLAG env to try */
	if (!dir) w0 &= ~0x0200;                            /* bit 9 = direction of the dominant axis (traced) */
	int ax2bit = getenv("AX2BIT") ? atoi(getenv("AX2BIT")) : 8;   /* direction bit of the second axis: 8 for the first remaining axis, 7 for the second ... (hypothesis) */
	if (!ax2dir) w0 &= (uint16_t)~(1u << ax2bit);
	long seg = 0; int gapped = 0;
	while (left > 0) {
		if (!strcmp(pattern, "zz") && seg >= 100) { seg = 0; w0 ^= 0x0200; printf("  reverse at %ld steps left (pos %d)\n", left, pos(axis) - p0); }
		if (!strcmp(pattern, "gap") && !gapped && left <= labs(steps) / 2) { gapped = 1; printf("  gap: no blocks for 300 ms (fifo %02x pos %d)\n", R(0x36), pos(axis) - p0); for (int i = 0; i < 15; i++) { usleep(20000); W(0x67, 0); } printf("  resume (fifo %02x pos %d)\n", R(0x36), pos(axis) - p0); }
		/* ramp: accelerate, cruise, decelerate so that v^2/2A fits in what is left */
		double vstop = sqrt(2 * A * (double)left);
		double target = vmax < vstop ? vmax : vstop;
		if (v < target) v += A * dt; if (v > target) v = target;
		if (v < 200) v = 200;
		long n = (long)(v * dt + 0.5); if (n < 1) n = 1; if (n > left) n = left;
		uint16_t blk[16] = { w0, 0x3020, seq, 0, (uint16_t)(v / 2), 0, (uint16_t)w6, 0, (uint16_t)n, 0, 0, 0, 0, 0, 0, 0 };
		if (ax2 >= 0) blk[ax2w] = (uint16_t)n;            /* the second axis' steps: which word? AX2W=10|12|14 */
		while (!dry && R(0x36) >= depth) { waits++; usleep(500); W(0x67, 0); }   /* FIFO at the chosen depth: wait for a slot */
		BLOCK(blk);
		left -= n; sent += n; blocks++; seg += n;
		W(0x67, 0);                                       /* the motion watchdog: bursts are accepted but never stepped without it (docs/fpga-motion.md); the vendor app writes it constantly (the shim filtered it out) */
		if (!dry && !t_first && pos(axis) != p0) t_first = now();
		if (!dry && blocks <= 12) printf("%8.4f block %d: %ld steps w4 %u -> fifo %02x pos %d\n", now() - t0, blocks, n, (unsigned)(v / 2), R(0x36), pos(axis) - p0);
		if (now() - hb > 0.1) { hb = now(); clock_const(); if (!dry) printf("%8.3f fifo %02x pos %d\n", now() - t0, R(0x36), pos(axis) - p0); }
	}
	printf("sent %ld steps in %d blocks (%d FIFO waits); fifo now %02x status %04x; draining\n", sent, blocks, waits, dry ? 0 : R(0x36), dry ? 0 : R(0x01));
	for (int i = 0; i < 300 && !dry; i++) {
		usleep(10000); W(0x67, 0);
		int32_t d = pos(axis) - p0;
		if (d && !t_first) t_first = now();
		if (d == steps) { t_last = now(); break; }        /* arrived: the motion time is first step -> last step */
		if (i % 10 == 0) { clock_const(); printf("%8.3f fifo %02x status %04x pos %d\n", now() - t0, R(0x36), R(0x01), d); }
	}
	usleep(100000);
	printf("position after: %d (moved %d, wanted %ld)\n", pos(axis), pos(axis) - p0, steps);
	/* --- the vendor's program end --- */
	select_all();
	static uint16_t end1[16] = { 0x0800, 0x4242, 0x0016, 0, 0, 0, 0x9c40, 0, 0x07d0, 0, 0x0760, 0x0002, 0x0001, 0, 0, 0 };
	static uint16_t end2[16] = { 0x0400, 0x0000, 0x0017, 0, 0x0001, 0, 0, 0, 0, 0x4000, 0, 0, 0xfd1c, 0x4242, 0x5af0, 0x0002 };
	if (!getenv("NOCTRL")) { BLOCK(end1); BLOCK(end2); }
	select_all();
	mode_all(0x20);
	usleep(50000);
	printf("position final: %d (moved %d)", pos(axis), pos(axis) - p0);
	if (ax2 >= 0) printf("; axis %d moved %d (word %d)", ax2, pos(ax2) - p2, ax2w);
	printf("; motion %.3f s (first -> last step), total %.3f s\n", t_last > t_first ? t_last - t_first : -1.0, now() - t0);
	return 0;
}
