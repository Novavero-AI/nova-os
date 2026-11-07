/* What a 32-bit process's launcher and its ntdll agree on
   (docs/spec/compat.md): the block the launcher writes into the process
   and passes as the first thread's second argument, and the address
   space below 4 GiB it lays out. Every field is a UINT32, so the
   launcher, built for x86-64, and the 32-bit ntdll lay it out alike. */

#ifndef NOVA_COMPAT_ABI_H
#define NOVA_COMPAT_ABI_H

#include "types.h"

/* The launcher's layout of a 32-bit process: the heap and the
   environment block, which holds the TEB in its first page, then the
   PEB, the loader data, the process parameters, their strings, the
   environment and the COMPAT_START block, both below 0x400000, where a
   seed chain program's image starts and runs on for as much as 128 MiB
   (observed in the chain); and the stack, the program's
   SizeOfStackReserve ending at COMPAT_STACK_TOP, where ntdll starts, at
   TinyCC's default for a DLL. Every range lies above USER_BASE and below
   4 GiB. */
#define COMPAT_HEAP_BASE 0x00100000u
#define COMPAT_HEAP_SIZE 0x00010000u
#define COMPAT_ENVIRONMENT_BASE 0x00300000u
#define COMPAT_ENVIRONMENT_SIZE 0x00002000u
#define COMPAT_STACK_TOP 0x10000000u

/* The process's view of storage is drive C:, whose root is its
   bootstrap directory (docs/spec/compat.md, Paths). */
#define COMPAT_VIEW_DRIVE 'C'

/* The block ntdll's start routine receives. current_directory is a
   NUL-terminated UTF-16 path with its drive and a trailing backslash:
   winternl.h documents no current-directory field in the process
   parameters, so ntdll keeps the one it is given here. */
typedef struct {
    UINT32 teb;
    UINT32 entry;      /* the program's entry point */
    UINT32 heap_base;
    UINT32 heap_size;
    UINT32 current_directory;
} COMPAT_START;

_Static_assert(sizeof(COMPAT_START) == 20, "every field is 32 bits");

#endif
