/* Resources: authority over a range of physical addresses, interrupt
   sources, bus requester IDs or named system privileges
   (docs/spec/ipc.md I6). The kernel creates one root resource of each
   kind for the first process; resource_create derives one covering a
   subrange of its parent's, of the parent's kind. */

#ifndef NOVA_RESOURCE_H
#define NOVA_RESOURCE_H

#include "object.h"

/* What a resource's range counts. No kind is 0. */
typedef enum {
    RESOURCE_MMIO = 1, /* physical addresses */
    RESOURCE_IRQ,      /* interrupt sources (native_abi.h) */
    RESOURCE_DMA,      /* bus requester IDs (ipc.md I8) */
    RESOURCE_SYSTEM    /* named system privileges */
} RESOURCE_KIND;

/* The rights a new resource's handle carries: every right a resource
   has. */
#define RESOURCE_RIGHTS (RESOURCE_RIGHT_USE | RIGHT_DUPLICATE | RIGHT_TRANSFER)

typedef struct {
    OBJECT object;
    RESOURCE_KIND kind;
    UINT64 base;
    UINT64 size;
} RESOURCE;

/* A root resource of kind over [base, base + size), with its creator's
   handle; or 0 if no page is free. */
RESOURCE *resource_create_root(RESOURCE_KIND kind, UINT64 base, UINT64 size);

/* A resource of parent's kind over [base, base + size), with its
   creator's handle. Returns OK; ERR_INVALID_ARGS if size is 0 or the
   range wraps; ERR_OUT_OF_RANGE if the range is not within parent's; or
   ERR_NO_MEMORY. */
STATUS resource_derive(const RESOURCE *parent, UINT64 base, UINT64 size,
                       RESOURCE **child);

/* Log a bus requester ID (ipc.md I8) as PCI writes a function's address,
   segment:bus:device.function in hexadecimal. */
void resource_log_requester(UINT64 requester);

/* Whether resource is of kind and covers [base, base + size). */
int resource_covers(const RESOURCE *resource, RESOURCE_KIND kind,
                    UINT64 base, UINT64 size);

#endif
