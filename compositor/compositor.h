/* What the compositor's parts share (docs/spec/display.md D2, D5 to D7
   and D19's phase 3 arrangement): the setup that takes the display
   (compositor.c) and the service of a connection and its window once it
   has (serve.c). */

#ifndef NOVA_COMPOSITOR_H
#define NOVA_COMPOSITOR_H

#include "compose.h"
#include "gfx.h"
#include "mapping.h"
#include "native.h"

/* What the service runs on: the compositor's mappings, which it makes
   its buffers' mappings in, the control channel the first process sends
   its messages on, the bootstrap channel the platform driver hands the
   input endpoints on, the screen and the wallpaper a frame is composed
   from, and memory for a band of TILE_ROWS rows of the screen's width. */
typedef struct {
    MAPPING *mapping;
    UINT32 control;
    UINT32 bootstrap;
    const SCREEN *screen;
    const COMPOSE_WALLPAPER *wallpaper;
    UINT32 *band;
} SERVE;

/* Serve the control channel's connections and their one window as they
   come, and the input endpoints on the bootstrap channel, until the first
   process closes the control channel or the platform driver the
   bootstrap channel, and then end the compositor. Ends it FAILED, logged,
   if the corners of a window cannot be made for this screen or a wait
   fails. */
_Noreturn void serve(const SERVE *context);

/* Log nova-compositor: event and status's number as its status key
   (docs/spec/log.md T1). */
void compositor_say_status(const char *event, UINT32 status);

/* Close the count handles a message carried. */
void compositor_close_received(const RECEIVED_HANDLE *handles, UINT32 count);

/* Drop a message of the control channel that came with status, the
   description's verdict, and the count handles it carried: logged, and
   for a message the description allows but this compositor does not
   serve at this point, with no status. */
void compositor_refuse(UINT32 status, const RECEIVED_HANDLE *handles,
                       UINT32 count);

#endif
