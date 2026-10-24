#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "display.h"

int display_open(display_t *d, const char *fbdev)
{
	memset(d, 0, sizeof *d);
	d->back.w = SCR_W; d->back.h = SCR_H;
	d->back.px = calloc(SCR_W * SCR_H, 2);
	d->front = calloc(SCR_W * SCR_H, 2);
	gfx_noclip(&d->back);
	if (!fbdev) return 0;
	int fd = open(fbdev, O_RDWR);
	if (fd < 0) { perror(fbdev); return 0; }
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && ioctl(fd, FBIOGET_FSCREENINFO, &f) == 0 &&
	    v.xres == SCR_W && v.yres == SCR_H && v.bits_per_pixel == 16 && f.line_length == SCR_W * 2) {
		void *m = mmap(NULL, SCR_W * SCR_H * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (m != MAP_FAILED) d->fbmem = m;
	} else fprintf(stderr, "%s: not 1024x600x16, drawing to RAM only\n", fbdev);
	close(fd);
	return 0;
}

void display_present(display_t *d)
{
	display_present_rows(d, 0, SCR_H);
	d->frame_no++;
}

void display_present_rows(display_t *d, int y0, int n)
{
	/* copy only the rows that changed: most frames change a few DRO digits */
	for (int y = y0; y < y0 + n && y < SCR_H; y++) {
		uint16_t *b = d->back.px + y * SCR_W, *f = d->front + y * SCR_W;
		if (!memcmp(b, f, SCR_W * 2)) continue;
		memcpy(f, b, SCR_W * 2);
		if (d->fbmem) memcpy(d->fbmem + y * SCR_W, b, SCR_W * 2);
	}
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void display_logo_caption(display_t *d, const char *text, const char *small)
{
	fb_t *f = &d->back;
	gfx_noclip(f);
	int x = 48, y = SCR_H - 92;
	gfx_text(f, &font_28, x + 2, y + 2, COL_BG1, text);   /* a soft shadow keeps it readable */
	gfx_text(f, &font_28, x, y, COL_TXT, text);
	if (small && small[0]) gfx_text(f, &font_14, x + 1, y + 40, COL_DIM, small);
	display_present(d);
}

bool display_logo(display_t *d, const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	uint8_t h[54];
	bool ok = fread(h, 1, sizeof h, f) == sizeof h && h[0] == 'B' && h[1] == 'M';
	uint32_t off = ok ? le32(h + 10) : 0;
	int32_t w = ok ? (int32_t)le32(h + 18) : 0, ht = ok ? (int32_t)le32(h + 22) : 0;
	int bpp = h[28] | h[29] << 8;
	ok = ok && w == SCR_W && (ht == SCR_H || ht == -SCR_H) && bpp == 24 && le32(h + 30) == 0;
	size_t row = ((size_t)SCR_W * 3 + 3) & ~(size_t)3;
	uint8_t *line = ok ? malloc(row) : NULL;
	if (!line || fseek(f, (long)off, SEEK_SET)) { free(line); fclose(f); return false; }
	static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
	for (int r = 0; r < SCR_H; r++) {
		if (fread(line, 1, row, f) != row) { free(line); fclose(f); return false; }
		int y = ht > 0 ? SCR_H - 1 - r : r;                 /* positive height = bottom-up rows */
		uint16_t *o = d->back.px + y * SCR_W;
		for (int x = 0; x < SCR_W; x++) {
			int t = bayer[y & 3][x & 3];                     /* 0..15: below one 5/6-bit step */
			int b = line[x * 3], g = line[x * 3 + 1], rr = line[x * 3 + 2];
			rr = rr + (t >> 1) > 255 ? 255 : rr + (t >> 1);   /* 5 bits: steps of 8 */
			g = g + (t >> 2) > 255 ? 255 : g + (t >> 2);      /* 6 bits: steps of 4 */
			b = b + (t >> 1) > 255 ? 255 : b + (t >> 1);
			o[x] = (uint16_t)((rr >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
		}
	}
	free(line);
	fclose(f);
	display_present(d);
	return true;
}
