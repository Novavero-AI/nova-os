/* What the logout disk's two programs share (driver.c, victim.c;
   ci/boot.py logout): the argument that makes a victim end at once, and
   the exit codes the programs end with. */

#ifndef NOVA_LOGOUT_H
#define NOVA_LOGOUT_H

/* A victim run with this argument ends with VICTIM_EXIT_ENDED at once;
   run with any other, its identifier, it beats until it is killed. */
#define VICTIM_ONCE "once"

#define VICTIM_EXIT_ENDED 0
#define VICTIM_EXIT_FAILED 1
#define DRIVER_EXIT_ENDED 0
#define DRIVER_EXIT_FAILED 1

#endif
