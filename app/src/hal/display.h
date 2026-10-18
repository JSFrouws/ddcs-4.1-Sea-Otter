/* Display: render into a RAM back buffer, then copy only changed rows to /dev/fb0
 * (1024x600 RGB565, mxsfb, stride 2048 - confirmed over SSH). Without a framebuffer
 * (host / --headless) the frame only lives in RAM and is served to the browser panel. */
#ifndef DISPLAY_H
#define DISPLAY_H
#include "../ui/gfx.h"

typedef struct {
	fb_t back;            /* draw here */
	uint16_t *front;      /* last presented frame (served by GET /frame) */
	uint16_t *fbmem;      /* mmap of /dev/fb0, NULL if none */
	uint32_t frame_no;
} display_t;

int display_open(display_t *d, const char *fbdev);   /* fbdev NULL = RAM only; returns 0 */
void display_present(display_t *d);
void display_present_rows(display_t *d, int y0, int n);   /* part of a frame (no frame_no bump) */

#endif
