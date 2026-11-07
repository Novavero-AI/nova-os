/* boot_switch(cr3, stack_top, entry, info): nova-boot's last step, from
   the firmware's page tables to the kernel's and into the kernel's entry.
   The operands arrive in RCX, RDX, R8 and R9 (Microsoft x64 convention).
   Of the lower half, the kernel's tables map only the pages from
   boot_switch to boot_switch_end, at the same address as the firmware's,
   so once CR3 is loaded this code reads no memory but its own
   instructions, and it leaves the firmware's stack, which the new tables
   do not map, before it pushes anything. */

/* The caller's home area for the callee's four register arguments. */
#define SHADOW_SPACE 32

    .text
    .globl boot_switch
    .globl boot_switch_end
boot_switch:
    movq %rcx, %cr3
    /* stack_top is page-aligned, so RSP is 16-byte aligned at the call
       with the home area above the return address. RBP is cleared so the
       kernel's frame chain ends here. */
    movq %rdx, %rsp
    subq $SHADOW_SPACE, %rsp
    xorl %ebp, %ebp
    movq %r9, %rcx
    call *%r8
1:
    cli
    hlt
    jmp 1b
boot_switch_end:
