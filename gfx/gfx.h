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

void gfx_clear(const SCREEN *screen);

/* Draw the wordmark and place the spinner beneath it. */
void gfx_boot_screen(const SCREEN *screen, SPINNER *spinner);

/* Draw the spinner's current frame and advance its head. */
void gfx_spinner_step(const SCREEN *screen, SPINNER *spinner);

#endif
