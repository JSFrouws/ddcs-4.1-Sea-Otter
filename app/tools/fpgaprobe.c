/*
 * fpgaprobe.c -- scripted FPGA experiments through the vendor driver (/dev/motion).
 * ddcsv4.out must be stopped (single opener). Build: arm-linux-gnueabi-gcc -static -O2.
 *
 * Usage: fpgaprobe CMD [CMD ...], executed in order, timestamps in ms since start:
 *   wAA=VVVV       write register                     rAA        read and print
 *   sMS            sleep MS ms                        d          dump read regs 0x00..0x7F (current select)
 *   D              dump with each axis selected (0..3) - finds banked read registers
 *   bAX:N          vendor jog burst: axis AX, N steps (negative = DIR bit clear), rate words as recorded
 *   B AX:N:RA:RB   same with rate words RA (0x12/13) and RB (0x14/15)
 *   pMS            poll for up to MS ms (~0.2 ms steps): print every change of 0x00, 0x01 and
 *                  the "watch" registers (default 0x1E-0x21), stop early 5 ms after busy clears
 *   WAA,BB,..      set the watch list for p
 *   iX             full init like our app (axis cfg + I/O ports of this machine)
 *   S              status sweep as the vendor does (0x2E=0, 0x00=0..3)
 *   x              per-axis read sweep: select a, read 0x00 0x01 and watch regs
 *   fFILE          replay "aa=vvvv" writes from FILE (e.g. the vendor init)
 *   FFILE          timed replay of a PANDORA_TRACE file ("ms W aa vvvv" lines; R lines are
 *                  re-issued as reads), heartbeat kept alive by the probe
 *   Q              quiet: don't print bursts (printing over SSH slows the bus traffic)
 *   q              spin until 0x01 reads 0 (no extra wait) - then the next command is immediate
 *   hMS            heartbeat (0x67=0) period during p/s, 0 = off (default 20)
 *   P              print the 4 position counters (0x02..0x09, hi word first)
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

static int fd;
static double t0;
static int quiet;
static int watch[16] = { 0x1E, 0x1F, 0x20, 0x21 }, nwatch = 4;

static double last_hb, hb_ms = 20;
static void W(unsigned a, unsigned v);
static void hb(void) { extern double now(void); if (hb_ms > 0 && now() - last_hb >= hb_ms) { W(0x67, 0); last_hb = now(); } }
double now(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1e3 + tv.tv_usec / 1e3; }
static void W(unsigned a, unsigned v) { uint32_t io[2] = { a, v }; ioctl(fd, 0x4620, io); }
static unsigned R(unsigned a) { uint32_t io[2] = { a, 0 }; ioctl(fd, 0x4621, io); return io[1] & 0xFFFF; }

static void burst(int ax, long n, unsigned ra, unsigned rb)
{
	unsigned long m = n < 0 ? -n : n;
	W(0, ax); W(0x12, ra & 0xFFFF); W(0x13, ra >> 16); W(0x14, rb & 0xFFFF); W(0x15, rb >> 16);
	W(0x0a, m & 0xFFFF); W(0x0b, m >> 16); W(0x03, 2); W(0x04, n > 0 ? 3 : 1); W(0x04, 0);
	printf("%9.3f burst ax%d %ld steps rate %04x/%04x\n", now() - t0, ax, n, ra, rb);
}

static void poll_regs(double ms)
{
	unsigned last[18] = { 0 };
	int n = 2 + nwatch;
	int first = 1;
	double start = now(), idle_since = -1;
	long loops = 0;
	while (now() - start < ms) {
		unsigned v[18];
		hb();
		v[0] = R(0); v[1] = R(1);
		for (int i = 0; i < nwatch; i++) v[2 + i] = R(watch[i]);
		loops++;
		if (first || memcmp(v, last, sizeof(unsigned) * n)) {
			printf("%9.3f  00=%04x 01=%04x", now() - t0, v[0], v[1]);
			for (int i = 0; i < nwatch; i++) printf(" %02x=%04x", watch[i], v[2 + i]);
			printf("%s\n", (v[1] & 0x200) ? "  BUSY" : "");
			memcpy(last, v, sizeof v); first = 0;
		}
		if (!(v[1] & 0x0F00)) { if (idle_since < 0) idle_since = now(); else if (now() - idle_since > 5) break; }
		else idle_since = -1;
	}
	printf("%9.3f  poll end (%ld loops, %.3f ms/loop)\n", now() - t0, loops, (now() - start) / loops);
}

static void dump(const char *tag)
{
	printf("%9.3f dump %s:", now() - t0, tag);
	for (int a = 0; a < 0x80; a++) { if (a % 16 == 0) printf("\n   %02x:", a); printf(" %04x", R(a)); }
	printf("\n");
}

static void init_like_app(void)
{
	const unsigned cfg[4] = { 6, 4, 4, 4 }, lim[4] = { 2, 3, 4, 0 }, alarm[4] = { 10, 11, 12, 13 };
	for (int a = 0; a < 4; a++) { W(0, a); W(2, cfg[a]); W(0, 0x0F); }
	for (int a = 0; a < 4; a++) {
		unsigned en = 1;
		W(0, a); W(0x36, alarm[a] << 8); W(0, a); W(1, en);
		en |= lim[a] ? 4 : 0x400; W(0, a); W(0x34, lim[a]); W(0, a); W(1, en);
		en |= lim[a] ? 8 : 0x800; W(0, a); W(0x34, lim[a] << 8 | lim[a]); W(0, a); W(1, en);
		en |= lim[a] ? 0 : 0x1000; W(0, a); W(0x35, lim[a]); W(0, a); W(1, en);
	}
	printf("%9.3f init done\n", now() - t0);
}

int main(int argc, char **argv)
{
	fd = open("/dev/motion", O_RDWR);
	if (fd < 0) { perror("open /dev/motion (stop ddcsv4.out first)"); return 1; }
	t0 = now();
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 1; i < argc; i++) {
		char *c = argv[i];
		unsigned a, v; int ax; long n; double ms; unsigned ra, rb;
		if (c[0] == 'w' && sscanf(c + 1, "%x=%x", &a, &v) == 2) { W(a, v); printf("%9.3f w %02x=%04x\n", now() - t0, a, v); }
		else if (c[0] == 'r' && sscanf(c + 1, "%x", &a) == 1) printf("%9.3f r %02x=%04x\n", now() - t0, a, R(a));
		else if (c[0] == 's' && sscanf(c + 1, "%lf", &ms) == 1) { double e = now() + ms; while (now() < e) { hb(); usleep(1000); } }
		else if (!strcmp(c, "d")) dump("");
		else if (!strcmp(c, "D")) for (int k = 0; k < 4; k++) { char t[8]; W(0, k); snprintf(t, sizeof t, "sel%d", k); dump(t); }
		else if (c[0] == 'b' && sscanf(c + 1, "%d:%ld", &ax, &n) == 2) burst(ax, n, 0x208d, 0x2710);
		else if (c[0] == 'B' && sscanf(c + 1, "%d:%ld:%x:%x", &ax, &n, &ra, &rb) == 4) burst(ax, n, ra, rb);
		else if (c[0] == 'p' && sscanf(c + 1, "%lf", &ms) == 1) poll_regs(ms);
		else if (c[0] == 'W') { nwatch = 0; for (char *p = c + 1; *p && nwatch < 16; ) { watch[nwatch++] = (int)strtol(p, &p, 16); if (*p == ',') p++; } }
		else if (!strcmp(c, "i")) init_like_app();
		else if (!strcmp(c, "S")) { W(0x2e, 0); for (int k = 0; k < 4; k++) W(0, k); printf("%9.3f sweep\n", now() - t0); }
		else if (!strcmp(c, "x")) for (int k = 0; k < 4; k++) {
			W(0, k);
			printf("%9.3f sel%d 00=%04x 01=%04x", now() - t0, k, R(0), R(1));
			for (int j = 0; j < nwatch; j++) printf(" %02x=%04x", watch[j], R(watch[j]));
			printf("\n");
		}
		else if (c[0] == 'F') {
			FILE *f = fopen(c + 1, "r");
			if (!f) { perror(c + 1); return 1; }
			unsigned long t, tf = 0; char d; int k = 0, first = 1; double start = now();
			while (fscanf(f, "%lu %c %x %x", &t, &d, &a, &v) == 4) {
				if (first) { tf = t; first = 0; }
				while (now() - start < (double)(t - tf)) hb();
				if (d == 'W') W(a, v); else (void)R(a);
				k++;
			}
			fclose(f);
			printf("%9.3f timed replay of %d accesses\n", now() - t0, k);
		}
		else if (!strcmp(c, "Q")) quiet = 1;
		else if (!strcmp(c, "q")) { long k = 0; while (R(1)) { hb(); k++; } printf("%9.3f idle after %ld polls\n", now() - t0, k); }
		else if (c[0] == 'h' && sscanf(c + 1, "%lf", &ms) == 1) { hb_ms = ms; printf("%9.3f heartbeat every %g ms\n", now() - t0, ms); }
		else if (c[0] == 'f') {
			FILE *f = fopen(c + 1, "r");
			if (!f) { perror(c + 1); return 1; }
			int k = 0;
			while (fscanf(f, "%x=%x", &a, &v) == 2) { W(a, v); k++; }
			fclose(f);
			printf("%9.3f replayed %d writes from %s\n", now() - t0, k, c + 1);
		}
		else if (!strcmp(c, "P")) {
			printf("%9.3f pos", now() - t0);
			for (int k = 0; k < 4; k++) { unsigned hi = R(3 + 2 * k), lo = R(2 + 2 * k); printf(" %c=%d", "XYZA"[k], (int)(hi << 16 | lo)); }
			printf("\n");
		}
		else { fprintf(stderr, "bad command: %s\n", c); return 2; }
	}
	return 0;
}
