#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
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

int file_replace(const char *path, const char *data, size_t len)
{
	FILE *f = fopen(path, "rb");
	if (f) {
		char *old = malloc(len + 1);
		size_t n = old ? fread(old, 1, len + 1, f) : 0;
		bool same = old && n == len && !memcmp(old, data, len);
		free(old); fclose(f);
		if (same) return 0;
	}
	char tmp[512];
	snprintf(tmp, sizeof tmp, "%s.new", path);
	f = fopen(tmp, "wb");
	if (!f) return -1;
	bool ok = fwrite(data, 1, len, f) == len && !fflush(f) && !fsync(fileno(f));
	if (fclose(f) || !ok) { remove(tmp); return -1; }
	return rename(tmp, path);
}
