/* Front panel: 18 keys + 7 LEDs on the TM1638 (docs/ui-controls-map.md).
 * Real: bit-banged from userspace through /dev/mem (the method proven by
 * mockup/ioctl-probe/tm1638read.c) - the vendor driver's MOTION_IOC_KEY returns nothing.
 * Sim: keys come from the browser panel / API, LEDs are just remembered. */
#ifndef PANEL_H
#define PANEL_H
#include "../common.h"

typedef struct panel panel_t;
struct panel {
	uint32_t (*scan)(panel_t *);          /* 32-bit key matrix: bit = byte*8 + bit of the 4 scan bytes */
	void (*leds)(panel_t *, uint8_t ui_led_mask);
	void *priv;
	uint32_t sim_keys;                    /* sim: currently held keys (same bit layout) */
	uint8_t led_state;
};

panel_t *panel_tm1638_open(void);         /* NULL if /dev/mem can't be mapped */
panel_t *panel_sim_open(void);

/* scan bit (byte*8 + bit) -> key id from ui.h (pkey_t), and back */
int panel_bit_to_key(int bit);
int panel_key_to_bit(int key);

#endif
