/* RDMSR and WRMSR for an MSR that may not exist. Both raise #GP for an
   MSR the processor does not implement, and WRMSR for a value with a
   reserved bit set (SDM Vol. 2B, RDMSR and WRMSR); a hypervisor can also
   lack an MSR its enumeration promises. The kernel's #GP handler
   continues a fault at msr_read_access or msr_write_access at
   msr_access_failed (idt.c), so the caller sees 0 instead of a halt.
   Microsoft x64 convention: the MSR in ECX, the second argument in RDX,
   the result in EAX; neither routine touches the stack, so the RET at
   msr_access_failed returns to the caller of either. RET and JMP are
   followed by INT3 (docs/spec/speculation.md M17). */

    .text

/* int msr_read_checked(UINT32 msr, UINT64 *value) */
    .globl msr_read_checked
msr_read_checked:
    movq %rdx, %r8
    .globl msr_read_access
msr_read_access:
    rdmsr
    shlq $32, %rdx
    movl %eax, %eax
    orq %rdx, %rax
    movq %rax, (%r8)
    movl $1, %eax
    ret
    int3

/* int msr_write_checked(UINT32 msr, UINT64 value) */
    .globl msr_write_checked
msr_write_checked:
    movl %edx, %eax
    shrq $32, %rdx
    .globl msr_write_access
msr_write_access:
    wrmsr
    movl $1, %eax
    ret
    int3

    .globl msr_access_failed
msr_access_failed:
    xorl %eax, %eax
    ret
    int3
