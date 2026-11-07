/* Drawing into a 32-bit linear framebuffer. nova-boot and the kernel share
   it, so the boot screen continues unchanged across the handoff. All
   arithmetic is integer: neither image links a C runtime. */

#ifndef NOVA_GFX_H
#define NOVA_GFX_H

#include "types.h"

/* EFI_GRAPHICS_PIXEL_FORMAT values for the two 32-bit layouts drawn. */
#define PIXEL_RGB 0
#define PIXEL_BGR 1

typedef struct {
    UINT32 *pixels;
    INT64 width;
    INT64 height;
    INT64 stride; /* pixels per scan line */
    UINT32 format;
} SCREEN;

/* The spinner's centre and size, in pixels and eighths of a pixel, and
   its head's angle, in sixteenths of a degree clockwise from twelve. */
typedef struct {
    INT64 cx, cy;
    INT64 radius, width;
    INT64 head;
} SPINNER;

/* Pixels from (left, top) up to but not including (right, bottom). */
typedef struct {
    INT64 left, top, right, bottom;
} RECT;

/* The screen's pixel value for the sRGB codes r, g and b, 0..255. */
UINT32 gfx_pixel(const SCREEN *screen, INT64 r, INT64 g, INT64 b);

/* The sRGB code whose light mixes the light of codes from and to, to's
   weighted by weight of total and from's by the rest. */
INT64 gfx_cross_fade(INT64 from, INT64 to, INT64 weight, INT64 total);

void gfx_clear(const SCREEN *screen);

/* Draw the wordmark and place the spinner beneath it. */
void gfx_boot_screen(const SCREEN *screen, SPINNER *spinner);

/* Draw the spinner's current frame and advance its head. */
void gfx_spinner_step(const SCREEN *screen, SPINNER *spinner);

/* Make a finished frame's stores visible. The kernel maps the framebuffer
   write-combining (docs/spec/boot-protocol.md), and stores to WC memory
   can wait in the processor's write-combining buffers until a serializing
   event such as SFENCE, so software must empty them itself where the data
   has to reach memory (SDM Vol. 3A 14.3 and 14.3.1). Every function here
   that completes a frame calls it, and so does any caller that writes
   pixels directly. */
void gfx_flush(void);

/* The part of the screen the boot screen draws on, clipped to the screen:
   every pixel outside it is black. */
void gfx_boot_bounds(const SCREEN *screen, const SPINNER *spinner,
                     RECT *bounds);

#endif
