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
   depends on state, as BACKGROUND_STATE says. */
typedef struct {
    BACKGROUND_STATE state;
    LIMB limb;
    RECT box;
    PIXEL_PAGE *image;
    PIXEL_PAGE *saved;
} BACKGROUND;

/* Set Limb up for the screen, take memory for the fade, and draw Limb
   into it offscreen, which takes seconds under emulation on a large
   screen. It reads nothing on the screen and writes nothing there, so
   the boot screen's spinner may turn meanwhile. Needs the page
   allocator. */
void background_prepare(const BOOT_INFO *info, BACKGROUND *background);

/* Show what background_prepare left: cross-fade the boot screen into
   Limb, or draw Limb at once, or nothing; free the memory prepared; and
   log, in one line, how long the preparation took, prepared, in
   nanoseconds, and the frames the boot screen's spinner started
   meanwhile, spinner_frames, which the caller measured, with the fade's
   frames and times, or why there was no fade. Needs a thread that may
   sleep, and nothing else drawing: the fade replaces the boot screen,
   spinner and all. */
void background_show(const BOOT_INFO *info, BACKGROUND *background,
                     UINT64 prepared, UINT64 spinner_frames);

#endif
