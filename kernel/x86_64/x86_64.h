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
#define FRAME_ERROR_CODE 128
#define FRAME_RIP 136
#define FRAME_CS 144
#define FRAME_RFLAGS 152
#define FRAME_RSP 160
#define FRAME_SS 168
#define FRAME_SIZE 176

/* PER_CPU's fields by byte offset. */
#define PER_CPU_KERNEL_STACK 0
#define PER_CPU_USER_RSP 8
#define PER_CPU_KERNEL_CR3 16
#define PER_CPU_USER_CR3 24
#define PER_CPU_CR3_NO_FLUSH 32
#define PER_CPU_ENTRY_STACK 40

/* IA32_SPEC_CTRL and IA32_PRED_CMD (Intel CPUID, "IA32_SPEC_CTRL MSR" and
   "IA32_PRED_CMD MSR"; APM 3.2.9): SPEC_CTRL's IBRS bit restricts
   indirect branch prediction (docs/spec/speculation.md M6), and writing
   PRED_CMD_IBPB to PRED_CMD is the indirect branch prediction barrier.
   No integer suffix: the assembly uses them too. */
#define IA32_SPEC_CTRL 0x48
#define IA32_PRED_CMD 0x49
#define PRED_CMD_IBPB 1

/* The measures of docs/spec/speculation.md M3 that the entry and exit
   carry where the processor needs them, one bit each. speculation.c
   decides which apply. */
#define MEASURE_IBRS 0x01      /* M6 rule 4: IBRS written on entry and exit */
#define MEASURE_IBPB 0x02      /* M9, M10: IBPB on entry */
#define MEASURE_RSB 0x04       /* M7: the 32-CALL overwrite on entry */
#define MEASURE_BHB 0x08       /* M8: the BHB-clearing sequence on entry */
#define MEASURE_VERW 0x10      /* M13: VERW before the return to user mode */
#define MEASURE_ISOLATION 0x20 /* M11: page-table isolation */

/* The 32-CALL overwrite's CALLs: the size of the return stack buffer,
   or return address predictor, that the vendors give (Intel HFB
   3.1.2.2.2, footnote 5; AMD STMS V2-3). */
#define RSB_ENTRIES 32
#define RETURN_ADDRESS_SIZE 8

#ifdef __ASSEMBLER__

/* A measure site: the code from MEASURE(m) to MEASURE_END runs only where
   measure m is in force. A branch on a flag set at boot would decide that
   too, but a branch can be mispredicted past the measure it guards, as
   the conditional SWAPGS shows (Intel SWAPGS); so each site is assembled
   behind an unconditional JMP over it, and speculation_init replaces the
   JMP with a NOP of the same length where m applies, before any entry
   from user mode. For a label not yet defined TinyCC encodes JMP as E9
   and a 32-bit displacement, five bytes (i386-asm.c, asm_opcode), which
   speculation_init checks before it writes. The site's address and m go
   to .rdata, between the file's MEASURE_TABLE and MEASURE_TABLE_END.
   Every site uses the labels 661 and 662, so sites do not nest. */
#define MEASURE(m)                                                     \
    661: jmp 662f;                                                     \
    .pushsection .rdata; .quad 661b, m; .popsection
#define MEASURE_END 662:

#define MEASURE_TABLE(name)                                            \
    .pushsection .rdata; .balign 8; .globl name; name:; .popsection
#define MEASURE_TABLE_END(name)                                        \
    .pushsection .rdata; .globl name; name:; .popsection

/* The 32-CALL overwrite (M7): RSB_ENTRIES near CALLs, each with a
   non-zero displacement, and no RET (Intel HFB 3.1.2.2.2, footnote 5;
   AMD STMS V2-3). Each pass makes two CALLs and drops the two return
   addresses they pushed, so the stack is as it was. INT3 after each CALL
   ends sequential speculation past it, and the LFENCE a mispredicted exit
   from the loop (AMD STMS G-5). Clobbers RAX and the flags. */
#define RSB_OVERWRITE                                                  \
    movl $(RSB_ENTRIES / 2), %eax;                                     \
    671: call 672f;                                                    \
    int3;                                                              \
    672: call 673f;                                                    \
    int3;                                                              \
    673: addq $(2 * RETURN_ADDRESS_SIZE), %rsp;                        \
    decl %eax;                                                         \
    jnz 671b;                                                          \
    lfence

/* IA32_SPEC_CTRL written with value, a variable of speculation.c: the
   IBRS writes of M6, rule 4, on entry and exit. Clobbers RAX, RCX and
   RDX. */
#define WRITE_SPEC_CTRL(value)                               \
    movl $IA32_SPEC_CTRL, %ecx;                              \
    movl value(%rip), %eax;                                  \
    xorl %edx, %edx;                                         \
    wrmsr

/* Steps 3 to 6 of M3's entry from user mode, once the frame is on the
   thread's kernel stack, and before any RET or indirect branch but the
   balanced CALLs and RETs of the last two: IBRS (M6), IBPB (M9, M10),
   the 32-CALL overwrite (M7) and the BHB-clearing sequence (M8). Each
   write to IA32_SPEC_CTRL or IA32_PRED_CMD waits for every older
   instruction and holds every younger one (Intel CPUID, "IA32_SPEC_CTRL
   MSR"). Clobbers RAX, RCX, RDX and the flags, which the frame holds. */
#define USER_ENTRY_MEASURES                                  \
    MEASURE(MEASURE_IBRS);                                   \
    WRITE_SPEC_CTRL(spec_ctrl_kernel);                       \
    MEASURE_END;                                             \
    MEASURE(MEASURE_IBPB);                                   \
    movl $IA32_PRED_CMD, %ecx;                               \
    movl $PRED_CMD_IBPB, %eax;                               \
    xorl %edx, %edx;                                         \
    wrmsr;                                                   \
    MEASURE_END;                                             \
    MEASURE(MEASURE_RSB);                                    \
    RSB_OVERWRITE;                                           \
    MEASURE_END;                                             \
    MEASURE(MEASURE_BHB);                                    \
    call bhb_clear;                                          \
    MEASURE_END

/* The exit's CR3 load under page-table isolation (M11), with scratch a
   register free to clobber: the user table, then cr3_no_flush added to
   the value, so that with PCIDs a later exit keeps the user table's
   translations; the per-CPU block lies in the entry area, which the
   user table maps. */
#define LOAD_USER_CR3(scratch)                               \
    movq %gs:PER_CPU_USER_CR3, scratch;                      \
    movq scratch, %cr3;                                      \
    movq %gs:PER_CPU_CR3_NO_FLUSH, scratch;                  \
    orq scratch, %gs:PER_CPU_USER_CR3

#else

#include <stddef.h>

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
   and IA32_KERNEL_GS_BASE holds while user code runs (S3). It lies in
   the CPU's entry area (gdt.c), which page-table isolation maps in every
   user table (docs/spec/speculation.md M11): the CR3 values are what the
   entry and the exit load there, and the entry stack is where the
   processor delivers an interrupt from user mode under isolation. */
typedef struct {
    UINT64 kernel_stack; /* the running thread's kernel stack top */
    UINT64 user_rsp;     /* the user's RSP across the system call */
    UINT64 kernel_cr3;   /* the running process's kernel table, as loaded */
    UINT64 user_cr3;     /* its user table, as the next exit loads it */
    UINT64 cr3_no_flush; /* what the exit adds to user_cr3 after loading */
    UINT64 entry_stack;  /* the entry stack's top */
} PER_CPU;

_Static_assert(offsetof(PER_CPU, user_rsp) == PER_CPU_USER_RSP &&
                   offsetof(PER_CPU, kernel_cr3) ==
                       PER_CPU_KERNEL_CR3 &&
                   offsetof(PER_CPU, user_cr3) == PER_CPU_USER_CR3 &&
                   offsetof(PER_CPU, cr3_no_flush) ==
                       PER_CPU_CR3_NO_FLUSH &&
                   offsetof(PER_CPU, entry_stack) ==
                       PER_CPU_ENTRY_STACK,
               "PER_CPU_ offsets");

/* A range of kernel addresses, page aligned, that page-table isolation
   maps in every user table. */
typedef struct {
    UINT64 start;
    UINT64 end;
} ENTRY_RANGE;

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

/* The code the processor runs under a user's page tables, page aligned
   at both ends: entry.S's and syscall.S's entry and exit code
   (docs/spec/speculation.md M11). The VERW operand lies in entry.S's,
   since the exit's VERW comes after its CR3 load (M13). */
extern const UINT8 entry_text[], entry_text_end[];
extern const UINT8 syscall_text[], syscall_text_end[];
extern const UINT16 verw_selector;

/* A measure site of entry.S or syscall.S (MEASURE): the address of the
   JMP that skips it, and the MEASURE_ bit under which it runs. */
typedef struct {
    UINT64 site;
    UINT64 measure;
} MEASURE_SITE;

extern const MEASURE_SITE entry_measures[], entry_measures_end[];
extern const MEASURE_SITE syscall_measures[], syscall_measures_end[];

/* switch.S: the 32-CALL overwrite of a process switch (M7), then a RET,
   which the overwritten return stack mispredicts into a trap. */
void rsb_overwrite(void);

/* msr.S: RDMSR and WRMSR for an MSR that may not exist, or may refuse
   the value: each returns 1 if the instruction completed and 0 if it
   raised #GP, which idt.c resumes at msr_access_failed when the faulting
   instruction is msr_read_access's or msr_write_access's. Under a
   hypervisor an MSR the enumeration promises can still be missing. */
int msr_read_checked(UINT32 msr, UINT64 *value);
int msr_write_checked(UINT32 msr, UINT64 value);
void msr_read_access(void);
void msr_write_access(void);
void msr_access_failed(void);

/* gdt.c: load the GDT and TSS, which live with the per-CPU block, the
   entry stack and the IST stacks in the CPU's entry area. */
void gdt_load(void);
PER_CPU *per_cpu_block(void);
ENTRY_RANGE gdt_entry_range(void);

/* Clear what one process left in the entry area, which page-table
   isolation maps in every user table, before another runs. */
void entry_area_wipe(void);

void idt_load(void);
ENTRY_RANGE idt_entry_range(void);
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

/* Load top into the TSS's RSP0, where the processor finds the stack for
   an interrupt from user mode (SDM Vol. 3A 7.14.4): the running thread's
   kernel stack top, or under page-table isolation the entry stack's. */
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

/* Where a page fault inside user_copy (syscall.S) continues, if rip is
   the instruction there that reaches user memory and address, the
   faulting address, lies below USER_TOP; 0 otherwise. */
UINT64 user_copy_fixup(UINT64 rip, UINT64 address);

#endif
#endif
