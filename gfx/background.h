/* The desktop background, faded in over the boot screen once the kernel
   has started. Kernel only: it takes memory from the page allocator, its
   pace from the kernel's clock, and logs on the serial port. */

#ifndef NOVA_BACKGROUND_H
#define NOVA_BACKGROUND_H

#include "boot_info.h"

/* Draw Limb offscreen, cross-fade the boot screen into it and log how
   long each took. Needs the page allocator, a thread that may sleep, and
   nothing else drawing: the fade replaces the boot screen, spinner and
   all. */
void background_fade_in(const BOOT_INFO *info);

#endif
