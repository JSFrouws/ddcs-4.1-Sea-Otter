/* Baked bitmap fonts (4-bit alpha), generated into fonts.c by tools/genfont.py. */
#ifndef FONTS_H
#define FONTS_H
#include <stdint.h>

typedef struct {
	uint8_t w, h;      /* bitmap size */
	int8_t x, y;       /* offset of the bitmap from the pen / line top */
	uint8_t adv;       /* pen advance */
	uint32_t off;      /* byte offset into bits; pixel i = nibble i (high first), row-major */
} glyph_t;

typedef struct {
	uint8_t height, ascent;
	uint16_t count;
	const glyph_t *g;
	const uint16_t *cp;   /* sorted code points, parallel to g */
	const uint8_t *bits;
} font_t;

extern const font_t font_12, font_14, font_16, font_20, font_28, font_40, font_mono;

#endif
