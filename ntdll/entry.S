/* ntdll's two entries from outside C: where a 32-bit process's first
   thread starts, and the 32-bit lane's gate (docs/spec/syscalls.md S11).
   TinyCC runs the C preprocessor over this file and assembles it as
   AT&T syntax. */

#include "native_abi.h"

/* UD2 (SDM Vol. 2B, UD2: 0F 0B). */
#define UD2 .byte 0x0f, 0x0b

    .text

/* ntdll's AddressOfEntryPoint, where the launcher starts the first
   thread, with ECX the bootstrap handle and EDX the COMPAT_START block's
   address, the two arguments a new thread starts with (S8, S11). The
   call does not return. */
    .globl ntdll_entry
ntdll_entry:
    pushl %edx
    pushl %ecx
    call ntdll_process_start
    UD2

/* UINT32 gate_call(UINT32 number, const UINT64 *arguments,
                    UINT32 *results), cdecl: INT to the gate with EAX the
   call number and EDX the arguments' address; on return EAX holds the
   status and EDX and ECX results 1 and 2, which go to results[0] and
   results[1] (S11). The kernel preserves EBX, ESI, EDI and EBP, as a
   called function preserves them in the x86 conventions ("Argument
   Passing and Naming Conventions"); EBX is saved here only to hold the
   results' address. */
    .globl gate_call
gate_call:
    movl 4(%esp), %eax
    movl 8(%esp), %edx
    int $COMPAT_GATE_VECTOR
    pushl %ebx
    movl 16(%esp), %ebx
    movl %edx, (%ebx)
    movl %ecx, 4(%ebx)
    popl %ebx
    ret
