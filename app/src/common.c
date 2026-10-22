#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "common.h"

const char AXIS_CH[NAXES] = { 'X', 'Y', 'Z', 'A' };

static bool virt;
static uint64_t virt_ms;

uint64_t now_ms(void)
{
	if (virt)
		return virt_ms;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

#include "perf.h"
perf_t PERF;

uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
}

void clock_use_virtual(bool on) { virt = on; virt_ms = 0; }
void clock_advance(uint32_t ms) { virt_ms += ms; }

struct dent64 { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; };

int dir_list(const char *dir, void (*fn)(const char *name, void *ctx), void *ctx)
{
	int fd = open(dir, O_RDONLY | O_DIRECTORY);
	if (fd < 0) return -1;
	char buf[4096] __attribute__((aligned(8)));
	long n;
	int count = 0;
	while ((n = syscall(SYS_getdents64, fd, buf, sizeof buf)) > 0)
		for (long p = 0; p < n;) {
			struct dent64 *e = (struct dent64 *)(buf + p);
			if (strcmp(e->name, ".") && strcmp(e->name, "..")) { fn(e->name, ctx); count++; }
			p += e->reclen;
		}
	close(fd);
	return n < 0 ? -1 : count;
}
