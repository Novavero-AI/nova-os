/* Access to a 32-bit linear framebuffer: a pixel's value in either of
   the two layouts drawn, and clearing. Portable C, so that a host
   program builds it to draw what an image draws; the store fence that
   makes the stores visible is flush.c's. */

#include "gfx.h"

UINT32 gfx_pixel(const SCREEN *screen, INT64 r, INT64 g, INT64 b)
{
    return (UINT32)(screen->format == PIXEL_BGR ? r << 16 | g << 8 | b
                                                : b << 16 | g << 8 | r);
}

void gfx_clear(const SCREEN *screen)
{
    INT64 x, y;

    for (y = 0; y < screen->height; y++)
        for (x = 0; x < screen->width; x++)
            screen->pixels[y * screen->stride + x] = 0;
}
