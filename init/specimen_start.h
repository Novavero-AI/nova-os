/* init's part in the type specimen (test/specimen.h): the display given
   to it, from /store, in place of the console. */

#ifndef NOVA_SPECIMEN_START_H
#define NOVA_SPECIMEN_START_H

#include "init.h"
#include "platform.h"

/* The boot file whose presence, whatever it holds, asks for the
   specimen. */
#define SPECIMEN_MARKER "SPECIMEN"

/* Once the kernel releases the display (display_released on
   bootstrap), start the specimen from its store path in the root job,
   with a view of its closure under /store and no arguments, and send it
   the display on its bootstrap channel after its launch message, as the
   console would be sent it (console.c). The console, which runs on,
   never gets the display. Logs and returns if any step fails. */
void specimen_start(const PLATFORM *platform, const STORE_SERVICES *services,
                    UINT32 bootstrap);

#endif
