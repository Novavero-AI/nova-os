/* What the kernel's calls return: OK, or an error code with the number
   and meaning docs/spec/syscalls.md S7 gives it (native_abi.h), so that
   a system call can pass the status on unchanged. */

#ifndef NOVA_STATUS_H
#define NOVA_STATUS_H

#include "native_abi.h"
#include "types.h"

typedef UINT32 STATUS;

#endif
