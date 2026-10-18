/* TM1638 over i.MX28 PINCTRL bank 1 (STB = GPIO1_17, DIO = GPIO1_18, CLK = GPIO1_19),
 * the same sequence as driver/motiondev_lld.c and mockup/ioctl-probe/tm1638read.c. */
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "panel.h"
#include "../ui/ui.h"

#define PINCTRL_PHYS 0x80018000u
#define DOUT(n) (0x700u + (n) * 0x10u)
#define DIN(n)  (0x900u + (n) * 0x10u)
#define DOE(n)  (0xB00u + (n) * 0x10u)
#define SET_ 0x4u
#define CLR_ 0x8u
#define SS   (1u << 17)
#define DIO  (1u << 18)
#define SCK  (1u << 19)

typedef struct { volatile uint8_t *pc; uint8_t leds[8]; } tm_t;

static inline void w(tm_t *t, uint32_t off, uint32_t v) { *(volatile uint32_t *)(t->pc + off) = v; }
static inline uint32_t r(tm_t *t, uint32_t off) { return *(volatile uint32_t *)(t->pc + off); }
static void dly(tm_t *t) { for (int i = 0; i < 50; i++) (void)r(t, DIN(1)); }
static void pin(tm_t *t, uint32_t bit, int hi) { w(t, DOUT(1) + (hi ? SET_ : CLR_), bit); dly(t); }

static void wbyte(tm_t *t, uint8_t v)
{
	w(t, DOE(1) + SET_, DIO);
	for (int i = 0; i < 8; i++) { pin(t, SCK, 0); pin(t, DIO, v & 1); pin(t, SCK, 1); v >>= 1; }
}

static uint8_t rbyte(tm_t *t)
{
	unsigned v = 0;
	for (int bit = 7; bit >= 0; bit--) {          /* 2-of-3 vote, as the vendor driver does */
		pin(t, SCK, 0); pin(t, SCK, 0);
		unsigned a = (r(t, DIN(1)) >> 18) & 1, b = (r(t, DIN(1)) >> 18) & 1, c = (r(t, DIN(1)) >> 18) & 1;
		v |= ((a == b) ? a : c) << bit;
		pin(t, SCK, 1);
	}
	return (uint8_t)v;
}

static uint32_t scan(panel_t *p)
{
	tm_t *t = p->priv;
	uint8_t s[4];
	pin(t, SS, 0);
	wbyte(t, 0x42);
	w(t, DOE(1) + CLR_, DIO); dly(t);
	for (int i = 0; i < 4; i++) s[i] = rbyte(t);
	pin(t, SS, 1);
	w(t, DOE(1) + SET_, DIO);
	return (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24;
}

static void cmd(tm_t *t, uint8_t c) { pin(t, SS, 0); wbyte(t, c); pin(t, SS, 1); }

static void leds(panel_t *p, uint8_t m)
{
	if (m == p->led_state) return;
	p->led_state = m;
	tm_t *t = p->priv;
	/* digit (odd address) and bit per LED, confirmed live (docs/ui-controls-map.md) */
	uint8_t d[8] = { 0 };
	if (m & LED_START) d[1] |= 1;
	if (m & LED_ETH) d[1] |= 2;
	if (m & LED_PAUSE) d[3] |= 1;
	if (m & LED_USB) d[3] |= 2;
	if (m & LED_RESET) d[5] |= 1;
	if (m & LED_SHIFT) d[5] |= 2;
	if (m & LED_SPINDLE) d[7] |= 1;
	cmd(t, 0x44);                                  /* fixed address mode */
	for (int a = 1; a < 8; a += 2) {
		pin(t, SS, 0); wbyte(t, (uint8_t)(0xC0 | a)); wbyte(t, d[a]); pin(t, SS, 1);
	}
	cmd(t, 0x8F);                                  /* display on */
}

panel_t *panel_tm1638_open(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) return NULL;
	void *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PINCTRL_PHYS);
	close(fd);
	if (m == MAP_FAILED) return NULL;
	panel_t *p = calloc(1, sizeof *p);
	tm_t *t = calloc(1, sizeof *t);
	t->pc = m;
	p->priv = t; p->scan = scan; p->leds = leds; p->led_state = 0xFF;
	leds(p, 0);
	return p;
}
