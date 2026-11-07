/* Resources: authority over a range of physical addresses, interrupt
   sources, I/O ports or named system privileges (docs/spec/ipc.md I6).
   This kernel creates one, the root system resource, whose range is the
   named privileges; in the first set the debug log is the only one. */

#ifndef NOVA_RESOURCE_H
#define NOVA_RESOURCE_H

#include "object.h"

/* The named system privileges, numbered from 0. */
#define SYSTEM_PRIVILEGE_DEBUG_LOG 0
#define SYSTEM_PRIVILEGES 1

typedef struct {
    OBJECT object;
    UINT64 base;
    UINT64 size;
} RESOURCE;

/* The root system resource, covering every named privilege, with its
   creator's handle; or 0 if no page is free. */
RESOURCE *resource_create_root_system(void);

/* Whether resource covers privilege. */
int resource_covers(const RESOURCE *resource, UINT64 privilege);

#endif
