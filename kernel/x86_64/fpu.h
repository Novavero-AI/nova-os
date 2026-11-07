/* The x87, SSE and AVX state of user threads, which the kernel preserves
   across system calls and switches (docs/spec/syscalls.md S4). Each user
   thread keeps its state, while switched out, in an area of
   FPU_AREA_ALIGNMENT-byte alignment that holds at least fpu_area_size()
   bytes. */

#ifndef NOVA_FPU_H
#define NOVA_FPU_H

#include "types.h"

/* XSAVE needs its area 64-byte aligned; FXSAVE 16 (SDM Vol. 1 13.4,
   10.5.1). */
#define FPU_AREA_ALIGNMENT 64

/* Enable the state components user code may use and choose how to save
   them: XSAVE with x87, SSE and AVX enabled in XCR0 where the processor
   has XSAVE, otherwise FXSAVE. Halts, with the reason, if the area that
   needs exceeds max_size bytes. */
void fpu_init(UINT64 max_size);

UINT64 fpu_area_size(void);

/* Fill area with the state a new thread starts with: x87 control word
   0x027F, MXCSR 0x1F80, every other register zero (S8). */
void fpu_state_init(UINT8 *area);

void fpu_save(UINT8 *area);
void fpu_restore(const UINT8 *area);

#endif
