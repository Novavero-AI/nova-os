/* Interrupt entry. Each installed vector's gate points at a stub here that
   pushes a zero where the CPU pushes no error code, then the vector, and
   joins interrupt_common, so every interrupt reaches interrupt_dispatch
   with the same INTERRUPT_FRAME (x86_64.h). From kernel mode the frame
   goes on the interrupted code's stack, which is safe because the
   Microsoft x64 convention keeps no data below RSP; from user mode the
   processor has switched to the stack the TSS's RSP0 names first (SDM
   Vol. 3A 7.14.4), the thread's kernel stack, or under page-table
   isolation the entry stack, from which the entry moves the frame to the
   thread's kernel stack. NMI, double fault and machine check switch to
   stacks of their own (gdt.c) and join ist_common instead.

   An entry from user mode carries the speculative-execution measures of
   docs/spec/speculation.md M3 in the order M3 gives, and the return to
   user mode those of its exit. Each is a measure site (x86_64.h,
   MEASURE), skipped where the processor does not need it. Everything
   here lies in the entry text, which page-table isolation maps in every
   user table (M11), since the processor runs it before the entry's CR3
   load and after the exit's. RET and JMP are followed by INT3 (M17).

   TinyCC runs the C preprocessor over this file and assembles it as AT&T
   syntax, where ';' separates statements; that lets one macro write a
   whole stub. */

#include "x86_64.h"
#include "native_abi.h"

/* The Microsoft x64 convention: the caller reserves 32 bytes above the
   return address for the callee's four register arguments. */
#define SHADOW_SPACE 32
#define VECTOR_AND_ERROR_CODE 16

/* Where the interrupted CS lies on entry to interrupt_common, above the
   vector, the error code and RIP. */
#define ENTRY_CS 24

/* What lies on the entry stack once the isolated entry has pushed RDI
   below the vector, and on the thread's stack once the isolated exit has
   pushed it below the CPU's frame: the offsets of the words it moves. */
#define SAVED_RDI 8
#define ENTRY_STACK_FRAME(field) (SAVED_RDI + (field) - FRAME_VECTOR)
#define EXIT_STACK_FRAME(field) (SAVED_RDI + (field) - FRAME_RIP)

/* SWAPGS (SDM Vol. 2B, SWAPGS: 0F 01 F8) and CLAC (Vol. 2A, CLAC: 0F 01
   CA), which TinyCC's assembler lacks. */
#define SWAPGS .byte 0x0f, 0x01, 0xf8
#define CLAC .byte 0x0f, 0x01, 0xca

#define PUSH_REGISTERS                                       \
    pushq %rax; pushq %rbx; pushq %rcx; pushq %rdx;          \
    pushq %rsi; pushq %rdi; pushq %rbp; pushq %r8;           \
    pushq %r9; pushq %r10; pushq %r11; pushq %r12;           \
    pushq %r13; pushq %r14; pushq %r15

#define POP_REGISTERS                                        \
    popq %r15; popq %r14; popq %r13; popq %r12;              \
    popq %r11; popq %r10; popq %r9; popq %r8;                \
    popq %rbp; popq %rdi; popq %rsi; popq %rdx;              \
    popq %rcx; popq %rbx; popq %rax

/* An exception's stub. TinyCC's assembler has no .if, so .rept, repeating
   zero times or once, pushes the placeholder for vectors outside
   ERROR_CODE_VECTORS. */
#define EXCEPTION_TO(vector, common)                         \
    exception_##vector:                                      \
    .rept 1 - ((ERROR_CODE_VECTORS >> (vector)) & 1);        \
    pushq $0;                                                \
    .endr;                                                   \
    pushq $(vector);                                         \
    jmp common;                                              \
    int3

#define EXCEPTION(vector) EXCEPTION_TO(vector, interrupt_common)
#define EXCEPTION_IST(vector) EXCEPTION_TO(vector, ist_common)

/* An external interrupt's stub; the CPU pushes no error code for one. */
#define INTERRUPT(name, vector)                              \
    .globl name;                                             \
    name:                                                    \
    pushq $0;                                                \
    pushq $(vector);                                         \
    jmp interrupt_common;                                    \
    int3

    MEASURE_TABLE(entry_measures)

    .text
    .balign PAGE_SIZE, 0xcc
    .globl entry_text
entry_text:

/* VERW's memory operand: the kernel's data selector, a writable data
   segment, as M13 decides; it lies in the entry text, which the user
   tables map, since the exit's VERW comes after its CR3 load. */
    .globl verw_selector
verw_selector:
    .word KERNEL_DATA_SELECTOR

EXCEPTION(0)
EXCEPTION(1)
EXCEPTION_IST(2)
EXCEPTION(3)
EXCEPTION(4)
EXCEPTION(5)
EXCEPTION(6)
EXCEPTION(7)
EXCEPTION_IST(8)
EXCEPTION(9)
EXCEPTION(10)
EXCEPTION(11)
EXCEPTION(12)
EXCEPTION(13)
EXCEPTION(14)
EXCEPTION(15)
EXCEPTION(16)
EXCEPTION(17)
EXCEPTION_IST(18)
EXCEPTION(19)
EXCEPTION(20)
EXCEPTION(21)
EXCEPTION(22)
EXCEPTION(23)
EXCEPTION(24)
EXCEPTION(25)
EXCEPTION(26)
EXCEPTION(27)
EXCEPTION(28)
EXCEPTION(29)
EXCEPTION(30)
EXCEPTION(31)
INTERRUPT(timer_stub, TIMER_VECTOR)
INTERRUPT(spurious_stub, SPURIOUS_VECTOR)
/* INT n pushes no error code (SDM Vol. 3A 7.13), so the 32-bit lane's gate
   takes the external interrupt's stub. */
INTERRUPT(compat_gate_stub, COMPAT_GATE_VECTOR)

/* One stub per message-signalled vector, each MSI_STUB_SIZE bytes from
   the last: two pushes, a jump and an INT3 take at most 13 bytes (a PUSH
   of an immediate above 0x7f needs 5, the jump 5), and .balign pads the
   rest. */
    .balign MSI_STUB_SIZE
    .globl msi_stubs
msi_stubs:
    .set msi_vector, MSI_VECTOR_FIRST
    .rept MSI_VECTOR_COUNT
    .balign MSI_STUB_SIZE, 0xcc
    pushq $0
    pushq $msi_vector
    jmp interrupt_common
    int3
    .set msi_vector, msi_vector + 1
    .endr

/* The CPU aligns RSP to 16 bytes before it pushes its frame (SDM Vol. 3A
   7.14.2), and INTERRUPT_FRAME is a multiple of 16 bytes, so RSP is
   aligned at the call, as the convention requires.

   An interrupt from user mode, whose saved CS has RPL 3, arrives with the
   user's GS base and the per-CPU block in IA32_KERNEL_GS_BASE; SWAPGS
   exchanges them, and the return to user mode exchanges them back
   (docs/spec/syscalls.md S3). From kernel mode GS is already the
   kernel's: the only kernel code that runs with the user's GS base is
   the system-call entry and exit, with interrupts disabled. The test
   that decides can be mispredicted either way, so each path fences
   before it can reach a GS-relative load (M5; Intel SWAPGS,
   "Mitigation"): the kernel path at once, the user path after the
   SWAPGS. Under isolation the CR3 load is serializing (SDM Vol. 3A
   10.3), but it needs the kernel's table, which a GS-relative load
   fetches first, so the LFENCE stays there too.

   Under isolation the processor has pushed its frame on the entry stack
   with the user's tables loaded. RDI is saved there, the kernel's tables
   are loaded, and the frame moves to the thread's kernel stack, where it
   lies as the processor would have pushed it without isolation.

   Delivery clears TF, NT, RF and IF but not AC (SDM Vol. 3A 7.12.1.3),
   which user code sets with POPF and user_copy sets on purpose. With
   SMAP on, AC set would let the handler, and any thread the scheduler
   switches to from it, reach user pages (S6), so every entry clears it;
   IRETQ restores the interrupted code's flags. CLAC raises #UD where the
   processor has no SMAP, and AC then has no effect at CPL 0. */
interrupt_common:
    testb $SELECTOR_RPL_MASK, ENTRY_CS(%rsp)
    jnz interrupt_from_user
    lfence
    PUSH_REGISTERS
    jmp interrupt_dispatch_common
    int3
interrupt_from_user:
    SWAPGS
    lfence
    MEASURE(MEASURE_ISOLATION)
    pushq %rdi
    movq %gs:PER_CPU_KERNEL_CR3, %rdi
    movq %rdi, %cr3
    movq %rsp, %rdi
    movq %gs:PER_CPU_KERNEL_STACK, %rsp
    pushq ENTRY_STACK_FRAME(FRAME_SS)(%rdi)
    pushq ENTRY_STACK_FRAME(FRAME_RSP)(%rdi)
    pushq ENTRY_STACK_FRAME(FRAME_RFLAGS)(%rdi)
    pushq ENTRY_STACK_FRAME(FRAME_CS)(%rdi)
    pushq ENTRY_STACK_FRAME(FRAME_RIP)(%rdi)
    pushq ENTRY_STACK_FRAME(FRAME_ERROR_CODE)(%rdi)
    pushq ENTRY_STACK_FRAME(FRAME_VECTOR)(%rdi)
    movq (%rdi), %rdi
    MEASURE_END
    PUSH_REGISTERS
    USER_ENTRY_MEASURES
interrupt_dispatch_common:
    cmpb $0, smap_enabled(%rip)
    je 2f
    CLAC
2:
    /* The convention expects the direction flag clear, and the
       interrupted code may have set it; IRETQ restores its RFLAGS. */
    cld
    movq %rsp, %rcx
    subq $SHADOW_SPACE, %rsp
    call interrupt_dispatch
    addq $SHADOW_SPACE, %rsp
    testb $SELECTOR_RPL_MASK, FRAME_CS(%rsp)
    jnz return_to_user
    POP_REGISTERS
    addq $VECTOR_AND_ERROR_CODE, %rsp
    iretq

/* The return to user mode through an INTERRUPT_FRAME at RSP, with
   interrupts disabled: from an interrupt, and on a new user thread's
   first switch (switch.S). IRETQ faults on a non-canonical RIP at CPL 0
   with the user's GS base already swapped in (docs/spec/syscalls.md S5),
   so a return address above USER_TOP, which only a kernel bug can
   produce, ends the thread's process instead (user_bad_return). Then
   come M3's exit measures: IBRS cleared (M6), the user's tables (M11)
   and VERW (M13), the last instruction before IRETQ. */
    .globl return_to_user
return_to_user:
    movq FRAME_RIP(%rsp), %rax
    cmpq user_top(%rip), %rax
    ja 3f
    MEASURE(MEASURE_IBRS)
    WRITE_SPEC_CTRL(spec_ctrl_user)
    MEASURE_END
    MEASURE(MEASURE_ISOLATION)
    jmp return_to_user_isolated
    int3
    MEASURE_END
    POP_REGISTERS
    addq $VECTOR_AND_ERROR_CODE, %rsp
    SWAPGS
    MEASURE(MEASURE_VERW)
    verw verw_selector(%rip)
    MEASURE_END
    iretq
3:
    movq %rsp, %rcx
    subq $SHADOW_SPACE, %rsp
    call user_bad_return
    int3

/* The same under page-table isolation. IRETQ pops its frame from the
   stack, which must be mapped once the user's tables are loaded, so the
   frame moves to the entry stack after the registers are restored, with
   the user's RDI, which is restored last, after the CR3 load that needs
   a register. A fault after that load, which only a kernel bug can
   raise, reaches kernel code the user's tables do not map, faults again
   until the entry stack runs out, and ends as a double fault. */
return_to_user_isolated:
    POP_REGISTERS
    addq $VECTOR_AND_ERROR_CODE, %rsp
    pushq %rdi
    movq %rsp, %rdi
    movq %gs:PER_CPU_ENTRY_STACK, %rsp
    pushq EXIT_STACK_FRAME(FRAME_SS)(%rdi)
    pushq EXIT_STACK_FRAME(FRAME_RSP)(%rdi)
    pushq EXIT_STACK_FRAME(FRAME_RFLAGS)(%rdi)
    pushq EXIT_STACK_FRAME(FRAME_CS)(%rdi)
    pushq EXIT_STACK_FRAME(FRAME_RIP)(%rdi)
    pushq (%rdi)
    LOAD_USER_CR3(%rdi)
    popq %rdi
    SWAPGS
    MEASURE(MEASURE_VERW)
    verw verw_selector(%rip)
    MEASURE_END
    iretq

/* NMI, double fault and machine check: each can arrive inside the
   system-call entry or exit, at CPL 0 with the user's GS base, so these
   never execute SWAPGS and never use GS (S3). Each can arrive with
   either table loaded and either IBRS value, so where isolation or
   M6's rule 4 applies the handler loads the kernel's table, from the
   per-CPU block whose address the IST stack keeps above the frame
   (gdt.c), and the kernel's IA32_SPEC_CTRL (M3). Each stops the kernel,
   so neither is restored. */
ist_common:
    PUSH_REGISTERS
    cld
    MEASURE(MEASURE_ISOLATION)
    movq FRAME_SIZE(%rsp), %rax
    movq PER_CPU_KERNEL_CR3(%rax), %rax
    movq %rax, %cr3
    MEASURE_END
    MEASURE(MEASURE_IBRS)
    WRITE_SPEC_CTRL(spec_ctrl_kernel)
    MEASURE_END
    movq %rsp, %rcx
    subq $SHADOW_SPACE, %rsp
    call exception_halt
    int3

/* The BHB-clearing sequence of M8: Intel's short sequence (Intel BHI,
   "Software BHB-clearing Sequence", Listing 1), which suffices on every
   processor before Alder Lake, as a routine the entries call. Its CALLs
   and RETs balance. INT3 follows each RET and JMP as M17 asks, where the
   listing has nothing, and in place of the NOP the listing puts after
   the JMP at 803, a byte that is never executed either way. Clobbers
   RAX, RCX and the flags. */
    .globl bhb_clear
bhb_clear:
    movl $5, %ecx
    call 801f
    jmp 805f
    int3
    .balign 64, 0xcc
801:
    call 802f
    ret
    int3
    .balign 64, 0xcc
802:
    movl $5, %eax
803:
    jmp 804f
    int3
804:
    subl $1, %eax
    jnz 803b
    subl $1, %ecx
    jnz 801b
    ret
    int3
805:
    lfence
    ret
    int3

    .balign PAGE_SIZE, 0xcc
    .globl entry_text_end
entry_text_end:

    MEASURE_TABLE_END(entry_measures_end)

    .data
    .balign 8
    .globl exception_stubs
exception_stubs:
    .quad exception_0, exception_1, exception_2, exception_3
    .quad exception_4, exception_5, exception_6, exception_7
    .quad exception_8, exception_9, exception_10, exception_11
    .quad exception_12, exception_13, exception_14, exception_15
    .quad exception_16, exception_17, exception_18, exception_19
    .quad exception_20, exception_21, exception_22, exception_23
    .quad exception_24, exception_25, exception_26, exception_27
    .quad exception_28, exception_29, exception_30, exception_31
