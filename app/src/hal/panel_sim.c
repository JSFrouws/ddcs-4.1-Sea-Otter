/* Simulated panel + the scan-bit <-> key table shared with the real one. */
#include <stdlib.h>
#include "panel.h"
#include "../ui/ui.h"

/* key code = byte*8 + bit + 1 (docs/ui-controls-map.md); here we use bit = code - 1 */
static const struct { int bit; pkey_t key; } MAP[] = {
	{ 7, K_F1 }, { 6, K_TAB }, { 5, K_SEL }, { 3, K_F2 }, { 2, K_SHIFT }, { 1, K_MOD },
	{ 15, K_F3 }, { 14, K_MODE }, { 13, K_CANCEL }, { 11, K_F4 }, { 10, K_YP }, { 9, K_ENTER },
	{ 23, K_F5 }, { 22, K_YM }, { 21, K_XP }, { 19, K_F6 }, { 31, K_F7 }, { 27, K_XM },
};

int panel_bit_to_key(int bit) { for (size_t i = 0; i < ARRAY_LEN(MAP); i++) if (MAP[i].bit == bit) return MAP[i].key; return K_NONE; }
int panel_key_to_bit(int key) { for (size_t i = 0; i < ARRAY_LEN(MAP); i++) if ((int)MAP[i].key == key) return MAP[i].bit; return -1; }

static uint32_t scan(panel_t *p) { return p->sim_keys; }
static void leds(panel_t *p, uint8_t m) { p->led_state = m; }

panel_t *panel_sim_open(void)
{
	panel_t *p = calloc(1, sizeof *p);
	p->scan = scan; p->leds = leds;
	return p;
}
