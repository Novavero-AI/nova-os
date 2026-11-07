/* gfx_flush (gfx.h): an x86-64 store fence, which only the images that
   draw on a framebuffer link, so that a host program can build the
   portable drawing code of gfx.c without it. */

#include "gfx.h"

void gfx_flush(void)
{
    __asm__ volatile("sfence" : : : "memory");
}
