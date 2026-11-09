/* init's part in the window test (test/wintest.h): starting the test
   client with a display connection on a disk that carries the flag file
   WINDOW. */

#ifndef NOVA_INIT_WINDOW_TEST_H
#define NOVA_INIT_WINDOW_TEST_H

#include "platform.h"

/* On a test disk that holds the flag file WINDOW, with a compositor
   running (compositor_wanted), start the window test client, wintest.exe
   from the boot volume, in the root job, with a connection the compositor
   issues for it (compositor_connect) on the SYSTEM layer, as the console's
   will be, and keep it running through the platform driver's supervisor
   (platform_keep). On any other boot, do nothing. Returns 1 if there is
   nothing to start or it started; 0, logged, if it did not. */
int window_test_start(const PLATFORM *platform);

#endif
