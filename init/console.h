/* init's part in the console (docs/spec/console.md C1, C2): the
   console's bootstrap message and control channel, the display's
   handoff once the kernel releases it, and sessions. */

#ifndef NOVA_INIT_CONSOLE_H
#define NOVA_INIT_CONSOLE_H

#include "console_protocol.h"
#include "platform.h"

/* Write the console's bootstrap message on channel, the end the platform
   driver keeps of the console's bootstrap channel: the debug log, the
   console's process with MAP, and the console's end of a new control
   channel, whose other end init keeps in place of the last console's.
   Returns the status of the first call that fails, with nothing it made
   left open, or OK. */
UINT32 console_bootstrap(const PLATFORM *platform, UINT32 process,
                         UINT32 channel);

/* The console console_bootstrap wrote for has started: once the display
   is released, send it on the control channel and give login, if it
   runs, a session on it. */
void console_started(const PLATFORM *platform);

/* After the startup report on bootstrap, init's bootstrap endpoint, wait
   for the kernel to release the display by closing its end, and close
   init's. Returns 1 once it is released, 0, logged, if the wait fails. */
int display_released(UINT32 bootstrap);

/* The message that gives a program the display (C1): the display's
   record in *message, and in *framebuffer a new MMIO resource covering
   exactly the framebuffer's pages, with the rights it travels with.
   Returns the status of resource_create. */
UINT32 display_message(const PLATFORM *platform,
                       CONSOLE_SETUP_DISPLAY_MESSAGE *message,
                       HANDLE_TRANSFER *framebuffer);

/* display_released, then send the display to the console, if one runs,
   and to each console started after. */
void console_start(const PLATFORM *platform, UINT32 bootstrap);

/* A new session on the console: send the console its end and return the
   client's, with CONSOLE_SESSION_RIGHTS, for a program; HANDLE_INVALID,
   logged, if there is no console or a call fails. */
UINT32 console_session(void);

#endif
