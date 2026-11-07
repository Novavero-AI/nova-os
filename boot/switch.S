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

/* CR4.PGE, bit 7 (SDM Vol. 3A 2.5), as page_table.h has it. */
#define CR4_GLOBAL_PAGES 0x80

    .text
    .globl boot_switch
    .globl boot_switch_end
boot_switch:
    movq %rcx, %cr3
    /* The firmware's tables, with their mapping of the framebuffer, are
       no longer loaded, and the kernel's, which map it write-combining,
       are; nothing here touches the framebuffer, so neither mapping is
       used before the flushes below. Loading CR3 keeps global
       translations; changing CR4.PGE invalidates every translation and
       paging-structure cache entry, for every PCID (SDM Vol. 3A
       5.10.4.1), which also drops any made under the firmware's PAT (SDM
       Vol. 3A 14.12.4), and changing it back restores the firmware's
       setting. Then WBINVD writes back and invalidates what the caches
       hold of the framebuffer through the firmware's mapping, whose type
       the MTRRs chose and could be cacheable, before the kernel's WC
       writes, which need not check the caches (SDM Vol. 3A 14.12.4; AMD
       APM Vol. 2 7.8.7, Table 7-12). */
    movq %cr4, %rax
    xorq $CR4_GLOBAL_PAGES, %rax
    movq %rax, %cr4
    xorq $CR4_GLOBAL_PAGES, %rax
    movq %rax, %cr4
    wbinvd
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
