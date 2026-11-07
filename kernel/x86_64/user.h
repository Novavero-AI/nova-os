/* User memory as the rest of the kernel sees it on x86-64: the range user
   mappings lie in, and the routines that check and copy user buffers
   (docs/spec/syscalls.md S5, S6). */

#ifndef NOVA_USER_H
#define NOVA_USER_H

#include "../status.h"

/* The lowest user address. The first 64 KiB are never mapped, so a null
   pointer access faults (S6). */
#define USER_BASE 0x10000ULL

/* USER_TOP: the top of the lower half less one page, which is never user
   memory, so that no instruction ends above it and the address SYSCALL
   saves is always canonical (S5). It depends on the paging depth, so it
   is set at boot. */
UINT64 user_space_top(void);

/* Whether size bytes at address lie in [USER_BASE, USER_TOP) without
   wrapping (S6). */
int user_range_valid(UINT64 address, UINT64 size);

/* Copy between a kernel buffer and size bytes of the calling thread's
   user memory at a user address. Returns OK, or ERR_BAD_ADDRESS if the
   range is not user memory or an access to it faults. */
STATUS copy_from_user(void *to, UINT64 from, UINT64 size);
STATUS copy_to_user(UINT64 to, const void *from, UINT64 size);

#endif
