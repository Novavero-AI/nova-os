/* The kernel's global descriptor table, in the layout of
   docs/spec/syscalls.md S2: the null descriptor, 64-bit kernel code,
   kernel data, 32-bit user code for compatibility mode, user data,
   64-bit user code and the 64-bit TSS. The firmware's table
   has its own layout (EDK2's CpuDxe puts 64-bit code at selector 0x38);
   with its own, the kernel names the selectors its interrupt gates and
   SYSCALL and SYSRET use. The TSS holds RSP0, the running thread's
   kernel stack top, and the interrupt stack table. */

#include "x86_64.h"

#define DESCRIPTOR_SIZE 8

/* Segment descriptor fields (SDM Vol. 3A 3.4.5, Figure 3-8, and Table 3-1
   for the type bits). */
#define DESC_LIMIT_FLAT (0xffffULL | 0xfULL << 48) /* limit bits 19:0 set */
#define DESC_READ_WRITE (1ULL << 41) /* code: readable; data: writable */
#define DESC_CODE (1ULL << 43)
#define DESC_CODE_OR_DATA (1ULL << 44) /* S: not a system descriptor */
#define DESC_DPL_USER (3ULL << 45)
#define DESC_PRESENT (1ULL << 47)
#define DESC_LONG (1ULL << 53)    /* L: 64-bit code */
#define DESC_DEFAULT_32 (1ULL << 54) /* D/B */
#define DESC_PAGE_GRANULAR (1ULL << 55) /* G: limit in 4 KiB units */

/* 64-bit mode ignores the base and limit (SDM Vol. 3A 3.2.4); every
   descriptor still describes a flat 4 GiB segment, as EDK2's do. The
   user descriptors differ from the kernel's only in their privilege
   level, except the 32-bit code descriptor: with L clear the processor
   runs its code in compatibility mode, and with D set that code's
   default operand and address size is 32 bits (SDM Vol. 3A 3.4.5,
   6.2.1, 12.8.5.3). Compatibility mode honours the base and limit, and
   the user data descriptor, which the same 32-bit code loads as DS, ES,
   FS and SS, is flat with D/B set, so its stack is 32-bit (3.2.4,
   3.4.5). No descriptor user code can load as SS is 16-bit, so no IRETQ
   returns to a stack that would take only the low 16 bits of RSP. */
#define KERNEL_CODE                                                         \
    (DESC_LIMIT_FLAT | DESC_READ_WRITE | DESC_CODE | DESC_CODE_OR_DATA |    \
     DESC_PRESENT | DESC_LONG | DESC_PAGE_GRANULAR)
#define KERNEL_DATA                                                         \
    (DESC_LIMIT_FLAT | DESC_READ_WRITE | DESC_CODE_OR_DATA | DESC_PRESENT | \
     DESC_DEFAULT_32 | DESC_PAGE_GRANULAR)
#define USER_CODE (KERNEL_CODE | DESC_DPL_USER)
#define USER_DATA (KERNEL_DATA | DESC_DPL_USER)
#define USER_CODE32                                                        \
    (DESC_LIMIT_FLAT | DESC_READ_WRITE | DESC_CODE | DESC_CODE_OR_DATA |   \
     DESC_DPL_USER | DESC_PRESENT | DESC_DEFAULT_32 | DESC_PAGE_GRANULAR)

/* The encodings EDK2's CpuDxe loads for its LINEAR_CODE64_SEL and
   LINEAR_DATA64_SEL, and the same with DPL 3. */
_Static_assert(KERNEL_CODE == 0x00af9a000000ffffULL, "kernel code");
_Static_assert(KERNEL_DATA == 0x00cf92000000ffffULL, "kernel data");
_Static_assert(USER_CODE == 0x00affa000000ffffULL, "user code");
_Static_assert(USER_DATA == 0x00cff2000000ffffULL, "user data");
_Static_assert(USER_CODE32 == 0x00cffa000000ffffULL, "32-bit user code");

/* A TSS descriptor in 64-bit mode is 16 bytes: the limit and the low 32
   bits of the base where a segment descriptor has them, type 1001b for an
   available 64-bit TSS, and the base's high 32 bits in the next eight
   bytes (SDM Vol. 3A 10.2.3, Figure 10-4, and Table 3-2). */
#define DESC_LIMIT_LOW 0xffffULL
#define DESC_LIMIT_HIGH_SHIFT 32 /* limit 19:16 to descriptor bits 51:48 */
#define DESC_LIMIT_HIGH 0xf0000ULL
#define DESC_BASE_LOW_SHIFT 16 /* base 23:0 to descriptor bits 39:16 */
#define DESC_BASE_LOW 0xffffffULL
#define DESC_BASE_MIDDLE_SHIFT 32 /* base 31:24 to descriptor bits 63:56 */
#define DESC_BASE_MIDDLE 0xff000000ULL
#define DESC_BASE_HIGH_SHIFT 32
#define DESC_TSS_AVAILABLE (0x9ULL << 40)

#define SELECTOR_INDEX(selector) ((selector) / DESCRIPTOR_SIZE)
#define GDT_ENTRIES (SELECTOR_INDEX(TSS_SELECTOR) + 2)

/* The null descriptor at 0 stays zero. Not const: loading a selector
   sets its descriptor's accessed bit (SDM Vol. 3A 3.4.5.1), and LTR
   marks the TSS busy. */
static UINT64 gdt[GDT_ENTRIES] = {
    [SELECTOR_INDEX(KERNEL_CODE_SELECTOR)] = KERNEL_CODE,
    [SELECTOR_INDEX(KERNEL_DATA_SELECTOR)] = KERNEL_DATA,
    [SELECTOR_INDEX(USER_CODE32_SELECTOR)] = USER_CODE32,
    [SELECTOR_INDEX(USER_DATA_SELECTOR)] = USER_DATA,
    [SELECTOR_INDEX(USER_CODE_SELECTOR)] = USER_CODE,
};

/* The 64-bit TSS (SDM Vol. 3A 10.7, Figure 10-11): RSP0 and the IST
   pointers are used. There is no I/O permission bit map yet. */
typedef struct {
    UINT32 reserved0;
    UINT64 rsp[3];
    UINT64 reserved1;
    UINT64 ist[7];
    UINT64 reserved2;
    UINT16 reserved3;
    UINT16 io_map_base;
} __attribute__((packed)) TSS;

_Static_assert(sizeof(TSS) == 104, "a 64-bit TSS is 104 bytes");

static TSS tss;

/* The handlers log and halt, so a fault taken inside one restarts its
   stack from the top and never returns onto the frame it overwrote. */
#define FAULT_STACK_SIZE 4096
#define STACK_ALIGNMENT 16

static UINT8 nmi_stack[FAULT_STACK_SIZE];
static UINT8 double_fault_stack[FAULT_STACK_SIZE];
static UINT8 machine_check_stack[FAULT_STACK_SIZE];

/* 64-bit mode loads CS only through a far transfer. This one is a far
   return to the next instruction; its REX.W form pops an eight-byte RIP
   and CS, where the plain form pops four bytes of each (SDM Vol. 2B, RET),
   and TinyCC's assembler has no mnemonic for it. */
#define FAR_RETURN_64 ".byte 0x48, 0xcb\n"

/* DS, ES, FS and GS start with the user data selector, which a new user
   thread starts with too; from then on each user thread keeps its own
   (context.c). 64-bit mode ignores their bases, except FS's and GS's, and
   their limits and attributes (SDM Vol. 3A 3.4.4), so the kernel loses
   nothing by them; and a return to user mode loads a null selector into
   any of them whose DPL is below 3 (SDM Vol. 3A 6.8.6), which this one's
   is not. GS's base is set afterwards: a segment load clears it (SDM
   Vol. 3A 3.4.4). */
static void reload_segments(void)
{
    __asm__ volatile("pushq %0\n\t"
                     "leaq 1f(%%rip), %%rax\n\t"
                     "pushq %%rax\n\t" FAR_RETURN_64 "1:\n\t"
                     "movw %w1, %%ss\n\t"
                     "movw %w2, %%ds\n\t"
                     "movw %w2, %%es\n\t"
                     "movw %w2, %%fs\n\t"
                     "movw %w2, %%gs"
                     :
                     : "i"(KERNEL_CODE_SELECTOR), "r"(KERNEL_DATA_SELECTOR),
                       "r"(USER_DATA_SELECTOR)
                     : "rax", "memory");
}

/* The CPU aligns RSP to 16 bytes as it delivers an interrupt in IA-32e
   mode, and the OS is expected to give IST pointers that alignment
   already (SDM Vol. 3A 7.14.2). */
static UINT64 stack_top(const UINT8 *stack)
{
    return ((UINT64)stack + FAULT_STACK_SIZE) & ~(UINT64)(STACK_ALIGNMENT - 1);
}

static void tss_load(void)
{
    UINT64 base = (UINT64)&tss;
    UINT64 limit = sizeof(tss) - 1;
    UINT32 index = SELECTOR_INDEX(TSS_SELECTOR);

    tss.ist[NMI_IST - 1] = stack_top(nmi_stack);
    tss.ist[DOUBLE_FAULT_IST - 1] = stack_top(double_fault_stack);
    tss.ist[MACHINE_CHECK_IST - 1] = stack_top(machine_check_stack);
    /* The I/O permission bit map would start at this offset, where the
       TSS ends, so it has none, and every port access at CPL 3 raises #GP
       (SDM Vol. 1 20.5.2). */
    tss.io_map_base = sizeof(tss);
    gdt[index] = (limit & DESC_LIMIT_LOW) |
                 (limit & DESC_LIMIT_HIGH) << DESC_LIMIT_HIGH_SHIFT |
                 (base & DESC_BASE_LOW) << DESC_BASE_LOW_SHIFT |
                 (base & DESC_BASE_MIDDLE) << DESC_BASE_MIDDLE_SHIFT |
                 DESC_TSS_AVAILABLE | DESC_PRESENT;
    gdt[index + 1] = base >> DESC_BASE_HIGH_SHIFT;
    __asm__ volatile("ltr %w0" : : "r"(TSS_SELECTOR) : "memory");
}

/* No LDT: a null selector in LDTR makes any reference to one raise #GP
   (APM 4.6.3), whatever firmware left there. */
void gdt_load(void)
{
    DESCRIPTOR_REGISTER gdtr;

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base = (UINT64)gdt;
    __asm__ volatile("lgdt %0" : : "m"(gdtr) : "memory");
    reload_segments();
    __asm__ volatile("lldt %w0" : : "r"(0) : "memory");
    tss_load();
}

void tss_set_kernel_stack(UINT64 top)
{
    tss.rsp[0] = top;
}
