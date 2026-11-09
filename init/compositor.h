/* init's part in the compositor (docs/spec/display.md D2, D3 and D19's
   phase 3 arrangement): whether one runs, its bootstrap message and
   control channel, its options, and the kernel's background and the
   display, which go to it once the kernel has released the display. The
   platform driver starts it, supervises it and makes it the input
   drivers' client while it runs (platform.c). */

#ifndef NOVA_INIT_COMPOSITOR_H
#define NOVA_INIT_COMPOSITOR_H

#include "init.h"
#include "platform.h"

/* Whether a compositor runs on this boot: on every disk but the specimen
   disk, which draws in the console's place. */
int compositor_wanted(void);

/* Take the kernel's background from *background, which then holds none,
   to give the compositor with its first start: init holds it from the
   kernel's message until then. */
void compositor_hold(BOOT_BACKGROUND *background);

/* After the startup report on bootstrap, init's bootstrap endpoint,
   wait for the kernel to release the display by closing its end, then
   have the platform driver start the compositor and the console beside
   it. Returns once they are started, or at once, logged, if the wait
   fails; the background held is closed then. */
void compositor_start(const PLATFORM *platform, UINT32 bootstrap);

/* Write the compositor's bootstrap message on channel, the end the
   platform driver keeps of its bootstrap channel: the debug log, its
   process with MAP, and its end of a new control channel, whose other
   end init keeps in place of the last compositor's. The first message on
   that channel is its options. Returns the status of the first call that
   fails before the bootstrap message is written, with nothing it made
   left open, or OK; a failure to send the options is logged. */
UINT32 compositor_bootstrap(const PLATFORM *platform, UINT32 process,
                            UINT32 channel);

/* The compositor compositor_bootstrap wrote for has started: send it the
   kernel's background, if init still holds it, and then the display with
   a resource covering exactly the framebuffer's pages; then the console,
   if one runs, its connection (console_connect). */
void compositor_started(const PLATFORM *platform);

/* Issue a connection to the compositor that runs (display.md D19's phase
   3 arrangement, rule 2): the label, no mark, the layers and the rights,
   on a channel of its own, and wait for the compositor's answer, or for
   its control channel to close, which is a compositor that has ended
   (rule 9). Returns OK, with *client the client's end of the connection,
   which the caller sends on to the client with DISPLAY_CONNECTION_RIGHTS;
   the status the compositor refused with, logged as nova-init: connection
   refused;
   or the status of the call that failed, ERR_PEER_CLOSED if the
   compositor has ended and ERR_BAD_STATE if none runs. *client is
   HANDLE_INVALID unless OK. */
UINT32 compositor_connect(const char *label, UINT32 layers, UINT32 rights,
                          UINT32 *client);

/* The compositor is given up: close the background init still holds, if
   it was never sent, and its control channel's end. */
void compositor_given_up(void);

#endif
