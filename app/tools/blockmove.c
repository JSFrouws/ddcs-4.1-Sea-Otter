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
	/* INIT_FILE=path: replay "addr value" (hex) lines first - the vendor-init table, raw or as
	 * Sea Otter substitutes it - at most INIT_N of them; INIT_ONLY=1 then reads reg 0 / 0x36 and
	 * exits without a stream. Bisecting which init write poisons the FPGA (2026-11-02: after the
	 * vendor app's init streams end cleanly, after Sea Otter's fpga_init the FPGA halts). */
	/* PREMODE=0x20|0x30: per-axis mode written BEFORE the init replay. A second Sea Otter init on
	 * an FPGA it already initialised halts every stream (2026-11-02, 5 of 5 app restarts, both
	 * builds); the app leaves the mode at 0x30+ch, the vendor app ends its streams with 0x20+ch. */
	if (getenv("PREMODE") && !dry) { unsigned pm = (unsigned)strtoul(getenv("PREMODE"), NULL, 0); mode_all((uint16_t)pm); printf("mode 0x%02x before the init\n", pm); }
	if (getenv("INIT_FILE") && !dry) {
		FILE *fi = fopen(getenv("INIT_FILE"), "r"); if (!fi) { perror(getenv("INIT_FILE")); return 1; }
		int n = 0, lim = getenv("INIT_N") ? atoi(getenv("INIT_N")) : 1 << 30; unsigned a, v;
		int us = getenv("INIT_US") ? atoi(getenv("INIT_US")) : 0;   /* INIT_US=n: pause n us after every write (the capture has no time stamps) */
		int p44 = getenv("INIT_44MS") ? atoi(getenv("INIT_44MS")) : 0;   /* INIT_44MS=n: pause n ms after every write to 0x44 (the vendor waits 51 ms after releasing it, vendor_start_reads.log t=3.1295) */
		char line[64];
		while (n < lim && fgets(line, sizeof line, fi)) {   /* "addr value [delay_us before this write]" */
			unsigned d = 0;
			if (line[0] == 'r') {                           /* "r addr 0 [delay]": one of the vendor's reads, replayed in place */
				if (sscanf(line + 1, "%x %x %u", &a, &v, &d) < 1) continue;
				if (d) usleep(d);
				R(a); n++; continue;
			}
			if (line[0] == 'd') {                           /* "d us": a pure pause in place of a read (is the read itself needed, or only the time it takes?) */
				if (sscanf(line + 1, "%u", &d) == 1 && d) usleep(d);
				n++; continue;
			}
			if (sscanf(line, "%x %x %u", &a, &v, &d) < 2) continue;
			if (d) usleep(d);
			W(a, (uint16_t)v); n++;
			if (us) usleep(us);
			if (p44 && a == 0x44) usleep(p44 * 1000);
		}
		fclose(fi);
		printf("replayed %d init writes: reg 00 = %04x, 0x36 = %03x, status %04x\n", n, R(0x00), R(0x36) & 0x1FF, R(0x01));
		if (getenv("INIT_ONLY")) return 0;
	}
	int dir = steps < 0 ? 0 : 1; long left = labs(steps);
	int32_t p0 = pos(axis), p2 = ax2 >= 0 ? pos(ax2) : 0;
	double t_first = 0, t_last = 0;
	printf("axis %d: %ld steps at %.0f steps/s, accel %.0f, position before %d\n", axis, steps, vmax, A, p0);

	/* --- the vendor's program start --- */
	W(0x0c, 0x0190); W(0x0d, 0); W(0x32, 1); W(0x32, 0);   /* as traced (0x190 = 400 = steps/mm?) */
	mode_all(0x30);
	/* the vendor's two end-of-program control blocks (14 ms after the done bit in its log) */
	static uint16_t end1[16] = { 0x0800, 0x4242, 0x0016, 0, 0, 0, 0x9c40, 0, 0x07d0, 0, 0x0760, 0x0002, 0x0001, 0, 0, 0 };
	static uint16_t end2[16] = { 0x0400, 0x0000, 0x0017, 0, 0x0001, 0, 0, 0, 0, 0x4000, 0, 0, 0xfd1c, 0x4242, 0x5af0, 0x0002 };
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
		while (!dry && R(0x36) >= depth) {                /* FIFO at the chosen depth: wait for a slot */
			waits++; usleep(500); W(0x67, 0);
			if (waits % 2000 == 0) printf("  FPGA not consuming: queue %02x status %04x pos %d after %d waits\n", R(0x36), R(0x01), pos(axis) - p0, waits);
			if (waits >= 6000) { printf("  giving up: the FPGA stopped playing\n"); goto done; }
		}
		BLOCK(blk);
		left -= n; sent += n; blocks++; seg += n;
		if (getenv("SEQINC")) seq++;                      /* word 2 as a running block counter: does the FPGA mind? */
		W(0x67, 0);                                       /* the motion watchdog: bursts are accepted but never stepped without it (docs/fpga-motion.md); the vendor app writes it constantly (the shim filtered it out) */
		if (!dry && !t_first && pos(axis) != p0) t_first = now();
		if (!dry && blocks <= 12) printf("%8.4f block %d: %ld steps w4 %u -> fifo %02x pos %d\n", now() - t0, blocks, n, (unsigned)(v / 2), R(0x36), pos(axis) - p0);
		if (now() - hb > 0.1) { hb = now(); clock_const(); if (!dry) printf("%8.3f fifo %02x pos %d\n", now() - t0, R(0x36), pos(axis) - p0); }
	}
	/* TAIL=n: one more block of n steps at the rate-word floor 0x64 (200 steps/s), as the app
	 * sends when a move's last remainder is a step or two (2026-11-02: the app's jog ended in a
	 * 1-step block and the FPGA halted right after; blockmove never sends fewer than v*dt steps) */
	if (getenv("TAIL") && !dry) {
		long n = atol(getenv("TAIL"));
		uint16_t blk[16] = { w0, 0x3020, seq, 0, 0x64, 0, (uint16_t)w6, 0, (uint16_t)n, 0, 0, 0, 0, 0, 0, 0 };
		while (R(0x36) >= depth) { usleep(500); W(0x67, 0); }
		BLOCK(blk); W(0x67, 0); sent += n; blocks++;
		printf("%8.4f tail block: %ld steps w4 0x64 -> fifo %02x\n", now() - t0, n, R(0x36));
	}
done:
	printf("sent %ld steps in %d blocks (%d FIFO waits); fifo now %02x status %04x; draining\n", sent, blocks, waits, dry ? 0 : R(0x36), dry ? 0 : R(0x01));
	/* ENDSWEEP=1: the vendor's end-of-stream acknowledge. In its log (docs/raw/2026-11-01_vendor-
	 * blocks.log) reg 0x00 reads 0x1000 the moment the last block finishes (14 ms BEFORE 0x36
	 * reads 0), and the app answers at once with R 01, W 2e=0, W 00=0, 1, 2, 3 - every time, 11
	 * times in the log. Sea Otter never does, and the FPGA flips to the halted 0x100 state ~60 ms
	 * after every drain. So: poll reg 0 while draining and idle, acknowledge as the vendor does. */
	int sweep = getenv("ENDSWEEP") ? atoi(getenv("ENDSWEEP")) : 0, acked = 0; unsigned r0_last = 0xffff;
	#define POLL_R0() do { unsigned r0 = R(0x00); if (r0 != r0_last) { printf("%8.3f reg 00 = %04x (fifo %02x)\n", now() - t0, r0, R(0x36) & 0x1FF); r0_last = r0; } \
		if (sweep && (r0 & 0x1000) && !acked) { R(0x01); W(0x2e, 0); W(0x00, 0); W(0x00, 1); W(0x00, 2); W(0x00, 3); acked = 1; printf("%8.3f acknowledged (2e=0, 00=0..3)\n", now() - t0); } \
		if ((r0 & 0x1000) && !done_acted) { done_acted = 1; \
			if (getenv("ENDMODE") && atoi(getenv("ENDMODE")) == 2) { mode_all(0x20); printf("%8.3f mode 0x20 at the done bit\n", now() - t0); } \
			if (getenv("X3E")) { W(0x3e, (uint16_t)atoi(getenv("X3E"))); printf("%8.3f 0x3e = %s\n", now() - t0, getenv("X3E")); }   /* the vendor: 3e=1 at +51 ms after its start block, 3e=0 at a program end */ \
			if (getenv("REOPEN")) { close(fd); fd = open("/dev/motion", O_RDWR); printf("%8.3f device closed and reopened at the done bit (fifo %02x)\n", now() - t0, R(0x36) & 0x1FF); } } } while (0)
	/* NOREAD_MS=n: no register READS for n ms after the drain (heartbeat + clock pair only), as the
	 * early runs and the gap test (queue empty 300 ms, no halt) had it; every idle watch that
	 * halted read 0x36/0x01 (lately 0x00) every few ms from the moment the queue emptied. */
	int noread = getenv("NOREAD_MS") ? atoi(getenv("NOREAD_MS")) : 0;
	int done_acted = 0;   /* ENDMODE=2 / REOPEN=1: act the instant the done bit (reg 0 bit 12) appears, as the vendor does at +14..46 ms */
	for (int i = 0; i < 3000 && !dry; i++) {
		usleep(1000); if (i % 10 == 0) W(0x67, 0);
		if (!noread) POLL_R0();
		int32_t d = pos(axis) - p0;
		if (d && !t_first) t_first = now();
		if (d == steps) { t_last = now(); break; }        /* arrived: the motion time is first step -> last step */
		if (i % 100 == 0) { clock_const(); printf("%8.3f fifo %02x status %04x pos %d\n", now() - t0, R(0x36), R(0x01), d); }
	}
	for (int i = 0; i < 100 && !dry; i++) { usleep(1000); if (!noread) POLL_R0(); }   /* the 100 ms the vendor's ack falls in */
	/* ENDCTRL=1: the vendor's end control blocks at the drain. Every run that halted had NOCTRL=1
	 * (no control blocks at all); the early runs that never halted wrote these ~100 ms after
	 * the drain. The flip comes ~150 ms after the drain in every run. */
	if (getenv("ENDCTRL")) { BLOCK(end1); BLOCK(end2); printf("%8.3f end control blocks written (fifo %02x)\n", now() - t0, R(0x36) & 0x1FF); }
	if (!noread) printf("position after: %d (moved %d, wanted %ld)\n", pos(axis), pos(axis) - p0, steps);
	/* IDLE_MS=n: stay idle as the app does after a move (device open, 0x67 every 20 ms, the
	 * app's reads) and watch 0x36 - Sea Otter saw it flip to 0x0100 ~250 ms after a jog
	 * ended (2026-11-01). ENDMODE=1: the vendor's end-of-stream mode 0x20+ch at the drain. */
	if (getenv("ENDMODE")) { mode_all(0x20); printf("  mode 0x20 written at drain\n"); }
	if (getenv("IDLE_MS")) {
		int ms = atoi(getenv("IDLE_MS")), hb_ms = getenv("HB_MS") ? atoi(getenv("HB_MS")) : 20, reads = getenv("READS") != NULL;
		double ti = now(); int flipped = 0;
		printf("  idle: heartbeat every %d ms, app-style reads %s\n", hb_ms, reads ? "on" : "off");
		for (int i = 0; i < ms / hb_ms; i++) {
			usleep(hb_ms * 1000);
			/* NOHB=1: no 0x67 after the drain. Every flip so far (20 ms, 5 ms, with the vendor's
			 * acknowledge) had the heartbeat running on an empty queue; the tool's early runs
			 * that never flipped stopped writing it the moment the position arrived. */
			if (!getenv("NOHB")) W(0x67, 0);
			/* the 0x23/0x24 pair every 100 ms, as during the stream (the vendor writes it every
			 * 125 ms, idle or not; every run that halted had NOCLOCK=1 and never wrote it) */
			if (i % (100 / hb_ms) == 0) clock_const();
			if ((now() - ti) * 1000 < noread) continue;   /* the read-free window */
			if (noread && reads != 2) { printf("%8.3f first read after the window: fifo %02x status %04x reg00 %04x pos %d\n", now() - t0, R(0x36) & 0x1FF, R(0x01), R(0x00), pos(axis) - p0); reads = 2; }
			POLL_R0();
			if (sweep == 2) { W(0x2e, 0); W(0x00, 0); W(0x00, 1); W(0x00, 2); W(0x00, 3); }   /* ENDSWEEP=2: acknowledge on every heartbeat */
			unsigned q = R(0x36) & 0x1FF, st = R(0x01);
			if (reads) { R(0x3D); R(0x37); R(0x0E); pos(axis); }
			if (q && !flipped) { flipped = 1; printf("  0x36 = %03x (status %04x) after %.0f ms idle\n", q, st, (now() - ti) * 1000); }
		}
		printf("  idle %d ms done: 0x36 = %03x status %04x\n", ms, R(0x36) & 0x1FF, R(0x01));
	}
	/* --- the vendor's program end --- */
	select_all();
	if (!getenv("NOCTRL") && !getenv("ENDCTRL")) { BLOCK(end1); BLOCK(end2); }
	select_all();
	mode_all(0x20);
	usleep(50000);
	printf("position final: %d (moved %d)", pos(axis), pos(axis) - p0);
	if (ax2 >= 0) printf("; axis %d moved %d (word %d)", ax2, pos(ax2) - p2, ax2w);
	printf("; motion %.3f s (first -> last step), total %.3f s\n", t_last > t_first ? t_last - t_first : -1.0, now() - t0);
	return 0;
}
