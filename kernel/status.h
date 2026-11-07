/* What the kernel's calls return: OK, or an error code with the number
   and meaning docs/spec/syscalls.md S7 gives it, so that a system call
   can pass the status on unchanged. */

#ifndef NOVA_STATUS_H
#define NOVA_STATUS_H

#include "types.h"

typedef UINT32 STATUS;

#define OK 0
#define ERR_INVALID_ARGS 2
#define ERR_NO_MEMORY 7
#define ERR_OUT_OF_RANGE 8
#define ERR_TIMED_OUT 10
#define ERR_BAD_STATE 14

#endif
