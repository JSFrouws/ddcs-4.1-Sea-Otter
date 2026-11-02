/* bus_t over the vendor driver /dev/motion (motiondev.ko from the stock firmware). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "bus.h"
#include "../perf.h"

#define MOTION_IOC_WRITE    0x4620u   /* {u32 addr; u32 data} */
#define MOTION_IOC_READ     0x4621u   /* {u32 addr; u32 data} in/out */
#define MOTION_IOC_BLOCK    0x4622u   /* u16[18]: 16 block words + 2 zero (as the vendor app) */
#define MOTION_IOC_BEEP_ON  0x4623u
#define MOTION_IOC_BEEP_OFF 0x4624u
#define MOTION_IOC_SPINDLE  0x4627u   /* s32 duty 0..3000 */

typedef struct { int fd; } io_t;

uint32_t bus_errors, bus_retries;
int bus_last_errno;

/* Register-level trace for diagnosis on the unit: if /ddcsv4/BUSLOG exists at start-up,
 * every write, read and block goes to /tmp/buslog.txt with a ms time stamp (the same
 * format as the iotrace captures in docs/raw, so the two can be diffed). Costs a line of
 * stdio per access; never leave the flag file on a working machine. */
static FILE *buslog;
static void bl(const char *kind, uint8_t a, uint16_t v)
{
	if (buslog) fprintf(buslog, "%llu %s %02x=%04x\n", (unsigned long long)now_us() / 1000, kind, a, v);
}

/* A rejected ioctl means the write never reached the FPGA: retry, and count it */
static void w(bus_t *b, uint8_t a, uint16_t v)
{
	bl("W", a, v);
	for (int k = 0; k < 5; k++) {
		uint32_t io[2] = { a, v };
		if (ioctl(((io_t *)b->priv)->fd, MOTION_IOC_WRITE, io) >= 0) return;
		bus_last_errno = errno;
		if (k) bus_retries++;
	}
	bus_errors++;
}

static uint16_t r(bus_t *b, uint8_t a)
{
	uint32_t io[2] = { a, 0 };
	if (ioctl(((io_t *)b->priv)->fd, MOTION_IOC_READ, io) < 0) {
		bus_last_errno = errno; bus_errors++;
		if (ioctl(((io_t *)b->priv)->fd, MOTION_IOC_READ, io) < 0) return 0xFFFF;
	}
	bl("R", a, (uint16_t)io[1]);
	return (uint16_t)io[1];
}

/* Block into the queue: the driver writes 0x50..0x5F and the 0x66 latch itself
 * (proven with tools/blockmove.c 2026-11-01). NOT retried like a write: whether a failed
 * 0x4622 can have latched the block is unknown, and a doubled block is a silent extra move.
 * A failure is counted; the position check at rest catches the lost steps. */
static void blk(bus_t *b, const uint16_t w16[16])
{
	uint16_t io[18] = { 0 };
	memcpy(io, w16, 16 * sizeof *io);
	if (buslog) {
		fprintf(buslog, "%llu B", (unsigned long long)now_us() / 1000);
		for (int i = 0; i < 16; i++) fprintf(buslog, " %04x", w16[i]);
		fputc('\n', buslog);
	}
	if (ioctl(((io_t *)b->priv)->fd, MOTION_IOC_BLOCK, io) < 0) { bus_last_errno = errno; bus_errors++; }
}

static void pwm(bus_t *b, int duty)
{
	int32_t d = CLAMP(duty, 0, 3000);
	ioctl(((io_t *)b->priv)->fd, MOTION_IOC_SPINDLE, &d);
}

static void beep(bus_t *b, bool on)
{
	ioctl(((io_t *)b->priv)->fd, on ? MOTION_IOC_BEEP_ON : MOTION_IOC_BEEP_OFF, 0);
}

static void settle(bus_t *b, uint32_t us) { (void)b; usleep(us); }

static void cl(bus_t *b)
{
	close(((io_t *)b->priv)->fd);
	free(b->priv);
	free(b);
}

bus_t *bus_ioctl_open(const char *dev)
{
	int fd = open(dev, O_RDWR);   /* single opener: ddcsv4.out must not be running */
	if (fd < 0)
		return NULL;
	if (access("/ddcsv4/BUSLOG", F_OK) == 0) { buslog = fopen("/tmp/buslog.txt", "w"); if (buslog) setvbuf(buslog, NULL, _IOLBF, 0); }
	bus_t *b = calloc(1, sizeof *b);
	io_t *p = calloc(1, sizeof *p);
	p->fd = fd;
	b->priv = p;
	b->write = w; b->read = r; b->block = blk; b->pwm = pwm; b->beep = beep; b->close = cl;
	b->settle = settle;
	return b;
}
