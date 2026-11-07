/* init as the starter and supervisor of the accounts service and login
   (docs/spec/accounts.md A1, A10). */

#ifndef NOVA_LOGIN_START_H
#define NOVA_LOGIN_START_H

#include "init.h"
#include "platform.h"

/* Start the accounts service and login from the nova-session store
   path, each in a job of its own, and keep both running through the
   platform driver's supervisor (platform_keep), with what services
   reaches: /store, /data and /users through the namespace, and the
   store service's control channel, through which login gets one of its
   own. With no /data or /users published, a machine with no data disk,
   neither starts. Returns 1 if both started; 0, logged, if not. */
int login_start(const PLATFORM *platform, const STORE_SERVICES *services);

/* A console has started: give login, if it runs, a session on it. */
void login_console_started(void);

#endif
