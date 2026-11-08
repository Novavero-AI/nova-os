/* The desktop background, drawn offscreen while the boot screen's
   spinner turns, handed on to the first process, and faded in over the
   boot screen once the first process has reported that its startup is
   done. Kernel only: it takes memory from the page allocator in
   sections, its pace from the kernel's clock, and logs on the serial
   port. */

#ifndef NOVA_BACKGROUND_H
#define NOVA_BACKGROUND_H

#include "boot_info.h"
#include "limb.h"
#include "object/section.h"

/* The bands of the largest screen limb_init takes, more than a message
   carries (native_abi.h). */
#define BACKGROUND_BANDS_MAX                                              \
    (((UINT64)LIMB_MAX_SIDE * LIMB_MAX_SIDE +                             \
      SECTION_MAX_SIZE / sizeof(UINT32) - 1) /                            \
     (SECTION_MAX_SIZE / sizeof(UINT32)))

/* An image in bands of memory sections, every one SECTION_MAX_SIZE bytes
   but the last, read one after another, row after row (native_abi.h):
   count bands, holding pages pages. The kernel holds each band's
   creator's handle until the image is freed. */
typedef struct {
    SECTION *bands[BACKGROUND_BANDS_MAX];
    UINT32 count;
    UINT64 pages;
} BANDS;

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
   depends on state, as BACKGROUND_STATE says; the bands' counts are 0
   where they hold none. */
typedef struct {
    BACKGROUND_STATE state;
    LIMB limb;
    RECT box;
    BANDS image;
    BANDS saved;
} BACKGROUND;

/* How the background's preparation went, which background_show logs:
   the nanoseconds the kernel took to draw Limb offscreen, drawn; those
   from the drawing's start to the first process's startup report,
   prepared, through which the boot screen's spinner turns; and the
   frames the spinner started over prepared. */
typedef struct {
    UINT64 drawn, prepared, spinner_frames;
} PREPARATION;

/* Set Limb up for the screen, take memory for the image and the fade,
   and draw Limb into the image offscreen, which takes seconds under
   emulation on a large screen. It reads nothing on the screen and
   writes nothing there, so the boot screen's spinner may turn
   meanwhile. Needs the page allocator. */
void background_prepare(const BOOT_INFO *info, BACKGROUND *background);

/* The label of what background_prepare left, in *label: Limb at the
   screen's size and in its format where the image holds it in no more
   sections than a message carries, else no background (native_abi.h).
   Returns how many of the image's bands the label hands on: all of
   them, or none. */
UINT32 background_label(const BOOT_INFO *info, const BACKGROUND *background,
                        BOOTSTRAP_BACKGROUND *label);

/* Show what background_prepare left: cross-fade the boot screen into
   Limb, or draw Limb at once, or nothing; close the kernel's handles to
   what it prepared; and log, in one line, the preparation, with the
   fade's frames and times, or why there was no fade. Needs a thread
   that may sleep, and nothing else drawing: the fade replaces the boot
   screen, spinner and all. */
void background_show(const BOOT_INFO *info, BACKGROUND *background,
                     const PREPARATION *preparation);

/* Close the kernel's handles to what background_prepare took, showing
   nothing. */
void background_discard(BACKGROUND *background);

#endif
