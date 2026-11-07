/* Declarations shared inside the x86-64 layer. entry.S includes this file
   too, for the vector numbers and the error-code mask; the C part is hidden
   from it. */

#ifndef NOVA_X86_64_H
#define NOVA_X86_64_H

/* Vectors 0-31 are the CPU's exceptions (SDM Vol. 3A, Table 7-1). */
#define EXCEPTION_COUNT 32
#define BREAKPOINT_VECTOR 3
#define DOUBLE_FAULT_VECTOR 8
#define PAGE_FAULT_VECTOR 14

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

#ifndef __ASSEMBLER__

#include "types.h"

/* A selector is its descriptor's byte offset in the GDT, with TI (bit 2,
   GDT or LDT) and RPL (bits 0-1) clear (SDM Vol. 3A 3.4.2). */
#define KERNEL_CODE_SELECTOR 0x08
#define KERNEL_DATA_SELECTOR 0x10
#define TSS_SELECTOR 0x18

/* The TSS's interrupt-stack-table slots, numbered from 1 as a gate names
   them (SDM Vol. 3A 7.14.5). A stack overflow faults on its guard page
   with the stack exhausted, and delivering a page fault on that same
   stack would fault again, into a double fault and then a triple fault
   (SDM Vol. 3A 7.15, Event 8), so both of those exceptions switch to a
   stack of their own. */
#define DOUBLE_FAULT_IST 1
#define PAGE_FAULT_IST 2

/* The operand of LGDT and LIDT: the table's size minus one, then its
   address (SDM Vol. 3A 3.5.1). */
typedef struct {
    UINT16 limit;
    UINT64 base;
} __attribute__((packed)) DESCRIPTOR_REGISTER;

/* The stack entry.S hands interrupt_dispatch, lowest address first: the
   registers it pushes, in reverse order; the vector; the error code, or 0
   where the CPU pushes none; and the frame the CPU pushes (SDM Vol. 3A
   7.14.2). */
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

/* The stubs in entry.S that the IDT's gates point at. */
extern const UINT64 exception_stubs[EXCEPTION_COUNT];
void timer_stub(void);
void spurious_stub(void);

void gdt_load(void);
void idt_load(void);
void apic_timer_start(UINT64 tsc_per_ms);
void apic_timer_interrupt(void);
void interrupt_dispatch(const INTERRUPT_FRAME *frame);

#endif
#endif
