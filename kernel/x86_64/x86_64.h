/* Declarations shared inside the x86-64 layer. entry.S and syscall.S
   include this file too, for the vector numbers, selectors and offsets;
   the C part is hidden from them. */

#ifndef NOVA_X86_64_H
#define NOVA_X86_64_H

/* Vectors 0-31 are the CPU's exceptions (SDM Vol. 3A, Table 7-1). */
#define EXCEPTION_COUNT 32
#define NMI_VECTOR 2
#define BREAKPOINT_VECTOR 3
#define INVALID_OPCODE_VECTOR 6
#define DOUBLE_FAULT_VECTOR 8
#define GENERAL_PROTECTION_VECTOR 13
#define PAGE_FAULT_VECTOR 14
#define MACHINE_CHECK_VECTOR 18

/* One bit per exception for which the CPU pushes an error code: 8, 10-14,
   17 and 21 (SDM Vol. 3A, Table 7-1) and 29, AMD's #VC. It is the mask
   EDK2's CpuExceptionHandlerLib uses (mErrorCodeFlag). No integer suffix:
   TinyCC's assembler evaluates it too. */
#define ERROR_CODE_VECTORS 0x20227d00

/* The timer's vector is in priority class 2, the lowest that software may
   use (SDM Vol. 3A 13.8.3). The spurious vector is all ones: the
   spurious-interrupt vector register's reset value, and P6-family APICs
   hardwire its low four bits to ones (SDM Vol. 3A 13.9). */
#define TIMER_VECTOR 0x20
#define SPURIOUS_VECTOR 0xff

/* A user thread of a 32-bit process enters the kernel with INT
   COMPAT_GATE_VECTOR (native_abi.h, docs/spec/syscalls.md S11). The
   kernel never routes an interrupt source to that vector, so an event on
   it is always the INT. */

/* The vectors message-signalled interrupts are given, one each, from
   priority class 3 up to the 32-bit lane's gate (SDM Vol. 3A 13.8.3).
   Their stubs in entry.S are MSI_STUB_SIZE bytes apart. */
#define MSI_VECTOR_FIRST 0x30
#define MSI_VECTOR_COUNT 0x50
#define MSI_STUB_SIZE 16

/* The vector slot of a frame built by the system-call entry, a value no
   interrupt or exception has. */
#define SYSCALL_FRAME_VECTOR 0x100

/* A selector is its descriptor's byte offset in the GDT, with TI (bit 2,
   GDT or LDT) clear and the requested privilege level in bits 0-1 (SDM
   Vol. 3A 3.4.2). The layout is docs/spec/syscalls.md S2's: SYSRET
   finds user data at IA32_STAR[63:48] + 8 and user code at + 16, so the
   user selectors carry RPL 3 and sit in that order. */
#define KERNEL_CODE_SELECTOR 0x08
#define KERNEL_DATA_SELECTOR 0x10
#define USER_CODE32_SELECTOR 0x1b
#define USER_DATA_SELECTOR 0x23
#define USER_CODE_SELECTOR 0x2b
#define TSS_SELECTOR 0x30
#define SELECTOR_RPL_MASK 3

/* The TSS's interrupt-stack-table slots, numbered from 1 as a gate names
   them (SDM Vol. 3A 7.14.5), as S2 assigns them. These three events can
   arrive while the processor runs at CPL 0 on a stack the user chose,
   inside the system-call entry and exit (S3, S5), so each switches to a
   stack of its own. */
#define NMI_IST 1
#define DOUBLE_FAULT_IST 2
#define MACHINE_CHECK_IST 3

/* INTERRUPT_FRAME's fields by byte offset, for the assembly. */
#define FRAME_R11 32
#define FRAME_R10 40
#define FRAME_RCX 96
#define FRAME_RAX 112
#define FRAME_VECTOR 120
#define FRAME_RIP 136
#define FRAME_CS 144
#define FRAME_RFLAGS 152
#define FRAME_RSP 160
#define FRAME_SIZE 176

/* PER_CPU's fields by byte offset. */
#define PER_CPU_KERNEL_STACK 0
#define PER_CPU_USER_RSP 8

#ifndef __ASSEMBLER__

#include "types.h"

/* The FS and GS base MSRs (APM 4.5.3): the running user thread's
   thread-local storage base (S10), and while the kernel runs, the
   per-CPU block (S3). */
#define IA32_FS_BASE 0xc0000100u
#define IA32_GS_BASE 0xc0000101u

/* The operand of LGDT and LIDT: the table's size minus one, then its
   address (SDM Vol. 3A 3.5.1). */
typedef struct {
    UINT16 limit;
    UINT64 base;
} __attribute__((packed)) DESCRIPTOR_REGISTER;

/* The stack entry.S hands interrupt_dispatch, lowest address first: the
   registers it pushes, in reverse order; the vector; the error code, or 0
   where the CPU pushes none; and the frame the CPU pushes (SDM Vol. 3A
   7.14.2). The system-call entry builds the same frame from what SYSCALL
   leaves in registers (syscall.S), and a user thread's first entry to
   user mode returns through one (user.c). */
typedef struct {
    UINT64 r15, r14, r13, r12, r11, r10, r9, r8;
    UINT64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    UINT64 vector;
    UINT64 error_code;
    UINT64 rip, cs, rflags, rsp, ss;
} INTERRUPT_FRAME;

/* entry.S keeps RSP 16-byte aligned at its call only while the frame is a
   whole number of 16-byte units. */
_Static_assert(sizeof(INTERRUPT_FRAME) % 16 == 0, "frame breaks alignment");
_Static_assert(sizeof(INTERRUPT_FRAME) == FRAME_SIZE, "FRAME_SIZE");

/* The processor's own record, which GS addresses while the kernel runs
   and IA32_KERNEL_GS_BASE holds while user code runs (S3). */
typedef struct {
    UINT64 kernel_stack; /* the running thread's kernel stack top */
    UINT64 user_rsp;     /* the user's RSP across the system-call entry */
} PER_CPU;

static inline int has_error_code(UINT64 vector)
{
    return vector < EXCEPTION_COUNT && (ERROR_CODE_VECTORS >> vector & 1);
}

/* The stubs in entry.S that the IDT's gates point at. */
extern const UINT64 exception_stubs[EXCEPTION_COUNT];
void timer_stub(void);
void spurious_stub(void);
void compat_gate_stub(void);

/* The stub for message-signalled vector v is at msi_stubs + (v -
   MSI_VECTOR_FIRST) * MSI_STUB_SIZE. */
extern const UINT8 msi_stubs[];

/* syscall.S: where SYSCALL from 64-bit and from compatibility mode
   enters the kernel. */
void syscall_entry(void);
void syscall_compat_entry(void);

void gdt_load(void);
void idt_load(void);
void apic_timer_start(UINT64 tsc_per_ms);
void apic_timer_interrupt(void);

/* Signal the end of the interrupt being handled to the local APIC (SDM
   Vol. 3A 13.8.5). */
void apic_eoi(void);

/* The message address and data that raise vector, edge-triggered with
   fixed delivery, at this processor's local APIC (SDM Vol. 3A 13.11). */
void apic_msi_message(UINT32 vector, UINT64 *address, UINT32 *data);
void interrupt_dispatch(INTERRUPT_FRAME *frame);

/* entry.S hands an NMI, double fault or machine check here, on its own
   stack; each stops the kernel. */
_Noreturn void exception_halt(const INTERRUPT_FRAME *frame);

/* Load the running thread's kernel stack top into the TSS's RSP0, where
   the processor finds it on an interrupt from user mode (SDM Vol. 3A
   7.14.4). */
void tss_set_kernel_stack(UINT64 top);

/* user.c: set up the processor for user mode and the system-call entry;
   the C sides of the system-call entry, of a fault in user mode and of
   the exit path's return-address check. */
void user_mode_init(void);
void syscall_dispatch(INTERRUPT_FRAME *frame);
void compat_syscall_dispatch(INTERRUPT_FRAME *frame);
_Noreturn void syscall_compat(void);
_Noreturn void user_exception(const INTERRUPT_FRAME *frame);
_Noreturn void user_bad_return(const INTERRUPT_FRAME *frame);
void per_cpu_set_kernel_stack(UINT64 top);

/* Where a page fault inside user_copy (syscall.S) continues, if rip is
   the instruction there that reaches user memory and address, the
   faulting address, lies below USER_TOP; 0 otherwise. */
UINT64 user_copy_fixup(UINT64 rip, UINT64 address);

#endif
#endif
