/*
 * iotrace2.c -- LD_PRELOAD shim for ddcsv4.out, like mockup/ioctl-probe/iotrace.c but it
 * also logs READS (0x4621), which the first shim didn't: every write, and every read whose
 * value changed for that (axis select, address) pair, plus the first read of each pair.
 * The goal is to see what the vendor app reads between jog bursts (done-ack, position).
 * Build: arm-linux-gnueabi-gcc -shared -fPIC -Os -fno-stack-protector -U_FORTIFY_SOURCE \
 *          -U_TIME_BITS -D_TIME_BITS=32 -o iotrace2.so iotrace2.c
 * Run:   cd /ddcsv4 && LD_PRELOAD=/ddcsv4/iotrace2.so ./ddcsv4.out &   -> /tmp/iotrace2.log
 * MPG_SPOOF=1 in the environment fakes a pendant (nobody at the machine): read 0x3D reports
 * dead-man held, axis 4, x10; read 0x0E is a scripted wheel: from t=8 s turn +40 counts per
 * 100 ms for 2 s, pause 3 s, turn -40/100 ms for 2 s, pause; repeats every 14 s.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

static int log_fd = -1;
static struct timeval t0;
static unsigned sel = 0xFF;
static uint16_t lastv[16][128];
static uint8_t seen[16][128];
static unsigned long nreads;

static void lg(const char *fmt, ...)
{
	char buf[256];
	struct timeval now;
	va_list ap;
	if (log_fd < 0) {
		log_fd = open("/tmp/iotrace2.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
		gettimeofday(&t0, NULL);
		if (log_fd < 0) return;
	}
	gettimeofday(&now, NULL);
	int n = snprintf(buf, sizeof buf, "t=%.4f  ", (now.tv_sec - t0.tv_sec) + (now.tv_usec - t0.tv_usec) / 1e6);
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof buf - n, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
	buf[n++] = '\n';
	write(log_fd, buf, n);
}

int ioctl(int fd, unsigned long cmd, ...)
{
	va_list ap;
	va_start(ap, cmd);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	long r = syscall(SYS_ioctl, fd, cmd, arg);
	if ((cmd & 0xFF00u) != 0x4600u || !arg) return (int)r;
	const uint32_t *p = arg;
	if (cmd == 0x4620u) {
		unsigned a = p[0] & 0xFF;
		if (a == 0) sel = p[1] & 0x0F;
		if (a != 0x67) lg("W %02x=%04x", a, p[1] & 0xFFFF);      /* 0x67 heartbeat is noise */
	} else if (cmd == 0x4621u) {
		unsigned a = p[0] & 0x7F, s = sel & 0x0F;
		static int spoof = -1;
		if (spoof < 0) spoof = getenv("MPG_SPOOF") != NULL;
		if (spoof && log_fd >= 0 && (a == 0x3D || a == 0x0E)) {
			struct timeval now;
			gettimeofday(&now, NULL);
			double t = (now.tv_sec - t0.tv_sec) + (now.tv_usec - t0.tv_usec) / 1e6 - 8;
			uint32_t *q = arg;
			if (a == 0x3D) q[1] = t > 0 ? 0x01BD : 0x01FF;   /* held, x10 (bit1), axis 4 (bit6) */
			else {
				double c = 0, k = t > 0 ? t - 14 * (int)(t / 14) : -1;
				int cycles = t > 0 ? (int)(t / 14) : 0;
				if (k >= 0 && k < 2) c = k * 400; else if (k >= 2 && k < 5) c = 800;
				else if (k >= 5 && k < 7) c = 800 - (k - 5) * 400; else c = 0;
				(void)cycles;
				q[1] = (uint16_t)(int)c;
			}
		}
		uint16_t v = p[1] & 0xFFFF;
		nreads++;
		if (!seen[s][a] || lastv[s][a] != v) lg("R %02x=%04x  (sel %x, read #%lu)", a, v, sel, nreads);
		seen[s][a] = 1; lastv[s][a] = v;
	} else if (cmd == 0x4622u) {
		const uint16_t *b = arg;
		lg("BLOCK %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x",
		   b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15], b[16], b[17]);
	}
	return (int)r;
}
