/* Wiping secrets from memory once they are used. */

#ifndef NOVA_WIPE_H
#define NOVA_WIPE_H

#include "types.h"

/* Overwrite size bytes at p with zeros.

   A wipe is a store to memory that is never read again, which is just
   what an optimizing compiler may delete. TinyCC at the pinned commit
   deletes no store: it generates code as it parses each statement and
   has no pass that removes stores, so this loop's stores are in the
   built kernel.exe as written. Each store also goes through a
   volatile-qualified lvalue, which an optimizing compiler treats as a
   volatile access and keeps, so the wipe does not depend on TinyCC
   staying simple.

   C reaches only memory it names. Copies the compiler keeps in registers,
   or spills to stack slots of its own, stay until something overwrites
   them. */
static inline void wipe(VOID *p, UINT64 size)
{
    volatile UINT8 *bytes = p;
    UINT64 i;

    for (i = 0; i < size; i++)
        bytes[i] = 0;
}

#endif
