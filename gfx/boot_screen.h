/* The boot screen nova-boot draws and the kernel keeps turning until it
   fades into the desktop background: the wordmark and a spinner beneath
   it. */

#ifndef NOVA_BOOT_SCREEN_H
#define NOVA_BOOT_SCREEN_H

#include "gfx.h"

/* The spinner's centre and size, in pixels and eighths of a pixel, and
   its head's angle, in sixteenths of a degree clockwise from twelve. */
typedef struct {
    INT64 cx, cy;
    INT64 radius, width;
    INT64 head;
} SPINNER;

/* Draw the wordmark and place the spinner beneath it. */
void gfx_boot_screen(const SCREEN *screen, SPINNER *spinner);

/* Draw the spinner's current frame and advance its head. */
void gfx_spinner_step(const SCREEN *screen, SPINNER *spinner);

/* The part of the screen the boot screen draws on, clipped to the screen:
   every pixel outside it is black. */
void gfx_boot_bounds(const SCREEN *screen, const SPINNER *spinner,
                     RECT *bounds);

#endif
