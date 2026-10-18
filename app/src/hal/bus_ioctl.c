/* bus_t over the vendor driver /dev/motion (motiondev.ko from the stock firmware). */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "bus.h"

#define MOTION_IOC_WRITE    0x4620u   /* {u32 addr; u32 data} */
#define MOTION_IOC_READ     0x4621u   /* {u32 addr; u32 data} in/out */
#define MOTION_IOC_BEEP_ON  0x4623u
#define MOTION_IOC_BEEP_OFF 0x4624u
#define MOTION_IOC_SPINDLE  0x4627u   /* s32 duty 0..3000 */

typedef struct { int fd; } io_t;

uint32_t bus_errors, bus_retries;
int bus_last_errno;

/* A rejected ioctl means the write never reached the FPGA: retry, and count it */
static void w(bus_t *b, uint8_t a, uint16_t v)
{
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
	return (uint16_t)io[1];
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
	bus_t *b = calloc(1, sizeof *b);
	io_t *p = calloc(1, sizeof *p);
	p->fd = fd;
	b->priv = p;
	b->write = w; b->read = r; b->pwm = pwm; b->beep = beep; b->close = cl;
	return b;
}
