/* Drawing into a 32-bit linear framebuffer. nova-boot and the kernel share
   it, so the boot screen continues unchanged across the handoff, and the
   console draws with it after (docs/spec/console.md). All arithmetic is
   integer: no image that draws links a C runtime. */

#ifndef NOVA_GFX_H
#define NOVA_GFX_H

#include "raster.h"

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

/* Pixels from (left, top) up to but not including (right, bottom). */
typedef struct {
    INT64 left, top, right, bottom;
} RECT;

/* The screen's pixel value for the sRGB codes r, g and b, 0..255. */
UINT32 gfx_pixel(const SCREEN *screen, INT64 r, INT64 g, INT64 b);

void gfx_clear(const SCREEN *screen);

/* Make a finished frame's stores visible. The framebuffer is mapped
   write-combining (docs/spec/boot-protocol.md), and stores to WC memory
   can wait in the processor's write-combining buffers until a serializing
   event such as SFENCE, so software must empty them itself where the data
   has to reach memory (SDM Vol. 3A 14.3 and 14.3.1). Every function that
   completes a frame calls it, and so does any caller that writes pixels
   directly. */
void gfx_flush(void);

#endif
