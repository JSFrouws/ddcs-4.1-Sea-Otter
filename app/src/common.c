#include <time.h>
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
