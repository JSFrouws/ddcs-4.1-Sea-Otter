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
