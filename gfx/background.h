/* The desktop background, faded in over the boot screen once the kernel
   has started. Kernel only: it takes memory from the page allocator, its
   pace from the kernel's clock, and logs on the serial port. */

#ifndef NOVA_BACKGROUND_H
#define NOVA_BACKGROUND_H

#include "boot_info.h"
#include "limb.h"

/* An offscreen image, a chain of pages (background.c). */
typedef struct PIXEL_PAGE PIXEL_PAGE;

/* What background_prepare left for background_show to do. */
typedef enum {
    /* Limb is drawn into image, and saved has room for the boot screen's
       part of the screen, box: fade the boot screen into Limb. */
    BACKGROUND_READY = 1,
    /* The page allocator had no room for image and saved: draw Limb on
       the screen at once, from limb. */
    BACKGROUND_NO_MEMORY,
    /* limb_init takes no screen this size: there is no background. */
    BACKGROUND_NO_SIZE
} BACKGROUND_STATE;

/* A background on its way to the screen. Which fields hold a value
   depends on state, as BACKGROUND_STATE says; drawn is how long drawing
   image took, in nanoseconds. */
typedef struct {
    BACKGROUND_STATE state;
    LIMB limb;
    RECT box;
    PIXEL_PAGE *image;
    PIXEL_PAGE *saved;
    UINT64 drawn;
} BACKGROUND;

/* Set Limb up for the screen, take memory for the fade, and draw Limb
   into it offscreen, which takes seconds under emulation on a large
   screen. It reads nothing on the screen and writes nothing there, so
   the boot screen's spinner may turn meanwhile. Needs the page
   allocator. */
void background_prepare(const BOOT_INFO *info, BACKGROUND *background);

/* Show what background_prepare left: cross-fade the boot screen into
   Limb, or draw Limb at once, or nothing; log how long each part took or
   why there is no background; and free the memory prepared. Needs a
   thread that may sleep, and nothing else drawing: the fade replaces the
   boot screen, spinner and all. */
void background_show(const BOOT_INFO *info, BACKGROUND *background);

#endif
