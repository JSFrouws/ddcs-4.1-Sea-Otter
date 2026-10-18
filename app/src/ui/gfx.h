/* Tiny RGB565 renderer: fills, short gradients, lines, triangles, anti-aliased text.
 * Everything draws into a back buffer; the display layer copies changed rows to /dev/fb0. */
#ifndef GFX_H
#define GFX_H
#include "../common.h"
#include "fonts.h"

#define SCR_W 1024
#define SCR_H 600
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

typedef struct { uint16_t *px; int w, h; int cx0, cy0, cx1, cy1; } fb_t;   /* clip rect [x0,x1) */

/* marine theme (16 bpp: keep gradients short - see docs/ui-hardware-constraints.md) */
#define COL_BG0   RGB(0x14, 0x40, 0x7a)
#define COL_BG1   RGB(0x06, 0x17, 0x2f)
#define COL_PANE  RGB(0x0f, 0x2d, 0x57)
#define COL_PANE2 RGB(0x0a, 0x22, 0x44)
#define COL_EDGE  RGB(0x24, 0x47, 0x78)
#define COL_EDGE2 RGB(0x35, 0x5f, 0x99)
#define COL_TXT   RGB(0xff, 0xff, 0xff)
#define COL_DIM   RGB(0x8f, 0xa7, 0xca)
#define COL_DIM2  RGB(0x5d, 0x78, 0x9f)
#define COL_CYAN  RGB(0x5c, 0xc8, 0xff)
#define COL_TEAL  RGB(0x3d, 0xdb, 0xb0)
#define COL_AMBER RGB(0xff, 0xb5, 0x47)
#define COL_RED   RGB(0xff, 0x5d, 0x5d)
#define COL_GREEN RGB(0x38, 0xd3, 0x9f)
#define COL_BLACK RGB(0, 0, 0)

void gfx_clip(fb_t *f, int x0, int y0, int x1, int y1);
void gfx_noclip(fb_t *f);
void gfx_fill(fb_t *f, int x, int y, int w, int h, uint16_t c);
void gfx_vgrad(fb_t *f, int x, int y, int w, int h, uint16_t top, uint16_t bot);
void gfx_hgrad(fb_t *f, int x, int y, int w, int h, uint16_t l, uint16_t r);
void gfx_rect(fb_t *f, int x, int y, int w, int h, uint16_t c);          /* 1 px outline */
void gfx_panel(fb_t *f, int x, int y, int w, int h, bool focus);         /* themed pane */
void gfx_line(fb_t *f, int x0, int y0, int x1, int y1, uint16_t c, int width);
void gfx_dash(fb_t *f, int x0, int y0, int x1, int y1, uint16_t c);
void gfx_tri(fb_t *f, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c);
void gfx_disc(fb_t *f, int cx, int cy, int r, uint16_t c);
void gfx_circle(fb_t *f, int cx, int cy, int r, uint16_t c);
uint16_t gfx_mix(uint16_t a, uint16_t b, int t16);                       /* t16 = 0..16 */
void gfx_dim(fb_t *f, int x, int y, int w, int h, int t16);             /* blend toward the background */

/* text: y = top of the line box; returns the width drawn. UTF-8. */
int gfx_text(fb_t *f, const font_t *fn, int x, int y, uint16_t c, const char *s);
int gfx_text_w(const font_t *fn, const char *s);
int gfx_text_r(fb_t *f, const font_t *fn, int xr, int y, uint16_t c, const char *s);   /* right-aligned */
int gfx_text_c(fb_t *f, const font_t *fn, int xc, int y, uint16_t c, const char *s);   /* centred */
void gfx_text_fit(fb_t *f, const font_t *fn, int x, int y, int maxw, uint16_t c, const char *s); /* ellipsis */

#endif
