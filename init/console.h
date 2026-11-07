/* init's part in the console (docs/spec/console.md C1, C2): the
   console's bootstrap message and control channel, the display's
   handoff once the kernel releases it, sessions, and the program that
   runs on the console. */

#ifndef NOVA_INIT_CONSOLE_H
#define NOVA_INIT_CONSOLE_H

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
   is released, send it on the control channel and start the demo client
   on a new session. */
void console_started(const PLATFORM *platform);

/* After the startup report on bootstrap, init's bootstrap endpoint, wait
   for the kernel to release the display by closing its end, close
   init's, then send the display to the console and start the demo
   client, if a console runs. */
void console_start(const PLATFORM *platform, UINT32 bootstrap);

/* A new session on the console: send the console its end and return the
   client's, with CONSOLE_SESSION_RIGHTS, for a program; HANDLE_INVALID,
   logged, if there is no console or a call fails. */
UINT32 console_session(void);

#endif
