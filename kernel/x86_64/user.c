/* User mode on x86-64: the processor set up for it, the C sides of the
   system-call entry and of the 32-bit lane's gate, faults from user mode,
   and the checks and copies of user memory (docs/spec/syscalls.md S1 to
   S6, S11). */

#include "user.h"
#include "x86_64.h"
#include "cpu.h"
#include "fpu.h"
#include "memory_layout.h"
#include "page_table.h"
#include "serial.h"
#include "../object/process.h"
#include "../syscall/syscall.h"

/* The SYSCALL MSRs, at the APM's addresses (APM 6.1.1, Figure 6-1;
   6.1.3), and the FS and GS base MSRs (APM 4.5.3). IA32_EFER.SCE enables
   SYSCALL and SYSRET (SDM Vol. 3A 2.2.1). */
#define IA32_STAR 0xc0000081u
#define IA32_LSTAR 0xc0000082u
#define IA32_CSTAR 0xc0000083u
#define IA32_FMASK 0xc0000084u
#define IA32_KERNEL_GS_BASE 0xc0000102u
#define EFER_SYSCALL (1ULL << 0)

/* SYSENTER_CS (APM 6.1.2, Figure 6-2). SYSENTER is illegal in long mode
   on AMD processors (APM 6.1.2) but enters the kernel from 64-bit user
   code on Intel's, provided this MSR holds a non-null selector (SDM Vol.
   3A 6.8.7.1); a null one makes it fault, so nova-os, which takes no
   SYSENTER (S1), loads 0 rather than trust what firmware left. */
#define IA32_SYSENTER_CS 0x174u

/* DR7 as reset leaves it, every breakpoint disabled (SDM Vol. 3A Table
   12-1, 20.2.4). S5 rests on user code being unable to arm a breakpoint
   that would raise #DB inside the system-call entry, on the user's
   stack; one left armed by firmware would. */
#define DR7_RESET 0x400ULL

/* IA32_STAR: SYSRET's selector base in bits 63:48, from which it loads
   SS at + 8 and 64-bit CS at + 16, and SYSCALL's CS in bits 47:32, from
   which it loads SS at + 8 (SDM Vol. 3A 6.8.8). The bases carry RPL 3
   and RPL 0 (APM 6.1.1). */
#define STAR_SYSRET_SHIFT 48
#define STAR_SYSCALL_SHIFT 32
#define SYSRET_SELECTOR_BASE (USER_DATA_SELECTOR - 8)
#define STAR                                                  \
    ((UINT64)SYSRET_SELECTOR_BASE << STAR_SYSRET_SHIFT |      \
     (UINT64)KERNEL_CODE_SELECTOR << STAR_SYSCALL_SHIFT)

_Static_assert(STAR == 0x001b000800000000ULL, "IA32_STAR as S2 gives it");
_Static_assert(SYSRET_SELECTOR_BASE + 16 == USER_CODE_SELECTOR,
               "SYSRET loads the user code selector");
_Static_assert(KERNEL_CODE_SELECTOR + 8 == KERNEL_DATA_SELECTOR,
               "SYSCALL loads the kernel data selector");

/* IA32_FMASK: RFLAGS bits SYSCALL clears, TF (8), IF (9), DF (10), NT
   (14) and AC (18), for the reasons S2 gives. */
#define SYSCALL_CLEARED_FLAGS 0x44700

/* CPUID.80000001H:EDX reports SYSCALL and SYSRET in bit 11 (SDM Vol. 3A
   6.8.8) and the execute-disable bit in bit 20 (SDM Vol. 3A 5.1.4);
   CPUID.07H.00H:EBX reports SMEP in bit 7 and SMAP in bit 20 (SDM Vol. 3A
   5.1.4). CPUID leaf 0 gives the highest basic leaf in EAX. */
#define CPUID_MAX_LEAF 0
#define CPUID_EXTENDED_FEATURES 0x80000001u
#define CPUID_EXTENDED_EDX_SYSCALL (1u << 11)
#define CPUID_EXTENDED_EDX_NX (1u << 20)
#define CPUID_STRUCTURED_FEATURES 7
#define CPUID_STRUCTURED_EBX_SMEP (1u << 7)
#define CPUID_STRUCTURED_EBX_SMAP (1u << 20)

/* CR4 (SDM Vol. 3A 2.5): TSD restricts RDTSC and RDTSCP to CPL 0 and
   PCE clear restricts RDPMC to it (docs/spec/replay.md); SMEP stops the
   kernel executing user pages and SMAP stops it reaching them except
   with RFLAGS.AC set (S6). */
#define CR4_TIME_STAMP_DISABLE (1ULL << 2)
#define CR4_PERFORMANCE_COUNTERS (1ULL << 8)
#define CR4_SMEP (1ULL << 20)
#define CR4_SMAP (1ULL << 21)

/* An address is canonical when its bits from 47 up, or from 56 up with
   5-level paging, are all equal (SDM Vol. 3A 5.1.1, 5.5.4), so the lower
   half ends at 2 to that power. */
#define LOWER_HALF_BITS_4_LEVEL 47
#define LOWER_HALF_BITS_5_LEVEL 56

/* Arguments 5 to 8 lie above the user's RSP at SYSCALL, past the stub's
   return address and its caller's 32-byte home area (S4). */
#define REGISTER_ARGUMENTS 4
#define STACK_ARGUMENTS_OFFSET 0x28

/* Read by entry.S and syscall.S. */
UINT64 user_top;
UINT8 smap_enabled;

static PER_CPU boot_per_cpu;

/* syscall.S. */
STATUS user_copy(void *to, const void *from, UINT64 size);
void user_copy_access(void);
void user_copy_fault(void);

/* ------------------------------------------------------------------------
   Setup. */

static void require(int present, const char *what)
{
    if (present)
        return;
    serial_write("nova: the processor lacks ");
    serial_write(what);
    serial_write(", which user mode needs, halted\r\n");
    halt_forever();
}

/* Without the execute-disable bit every page is executable (SDM Vol. 3A
   5.5.4), so no user page could be writable and not executable. */
static void check_processor(void)
{
    UINT32 a, b, c, d;

    cpuid(CPUID_EXTENDED_FEATURES, &a, &b, &c, &d);
    require(d & CPUID_EXTENDED_EDX_SYSCALL, "syscall and sysret");
    require((d & CPUID_EXTENDED_EDX_NX) &&
                (rdmsr(IA32_EFER) & EFER_NO_EXECUTE),
            "the execute-disable bit");
}

static void protect_kernel(void)
{
    UINT64 cr4 = (read_cr4() | CR4_TIME_STAMP_DISABLE) &
                 ~CR4_PERFORMANCE_COUNTERS;
    UINT32 a, b, c, d;

    wrmsr(IA32_SYSENTER_CS, 0);
    __asm__ volatile("mov %0, %%dr7" : : "r"(DR7_RESET));

    cpuid(CPUID_MAX_LEAF, &a, &b, &c, &d);
    if (a >= CPUID_STRUCTURED_FEATURES) {
        cpuid(CPUID_STRUCTURED_FEATURES, &a, &b, &c, &d);
        if (b & CPUID_STRUCTURED_EBX_SMEP)
            cr4 |= CR4_SMEP;
        if (b & CPUID_STRUCTURED_EBX_SMAP) {
            cr4 |= CR4_SMAP;
            smap_enabled = 1;
        }
    }
    write_cr4(cr4);
    serial_write("nova: user mode: smep ");
    serial_write(cr4 & CR4_SMEP ? "on" : "absent");
    serial_write(", smap ");
    serial_write(smap_enabled ? "on" : "absent");
    serial_write(", rdtsc and rdpmc kernel only\r\n");
}

/* A thread's vector state is kept in a page of its own (process.c). GS
   addresses the per-CPU block from here on; IA32_KERNEL_GS_BASE holds
   the user's GS base, 0, until SWAPGS exchanges them on the way to user
   mode (S2, S3). */
void user_mode_init(void)
{
    int lower_bits = paging_levels() == FIVE_LEVELS ? LOWER_HALF_BITS_5_LEVEL
                                                    : LOWER_HALF_BITS_4_LEVEL;

    check_processor();
    protect_kernel();
    fpu_init(PAGE_SIZE);
    user_top = (1ULL << lower_bits) - PAGE_SIZE;
    wrmsr(IA32_GS_BASE, (UINT64)&boot_per_cpu);
    wrmsr(IA32_KERNEL_GS_BASE, 0);
    wrmsr(IA32_STAR, STAR);
    wrmsr(IA32_LSTAR, (UINT64)syscall_entry);
    wrmsr(IA32_CSTAR, (UINT64)syscall_compat_entry);
    wrmsr(IA32_FMASK, SYSCALL_CLEARED_FLAGS);
    wrmsr(IA32_EFER, rdmsr(IA32_EFER) | EFER_SYSCALL);
    serial_write("nova: user mode: syscall entry ready, user memory ");
    serial_hex(USER_BASE);
    serial_write(" to ");
    serial_hex(user_top);
    serial_write("\r\n");
}

void per_cpu_set_kernel_stack(UINT64 top)
{
    boot_per_cpu.kernel_stack = top;
}

/* ------------------------------------------------------------------------
   The system-call entry's C side. */

/* A stack argument's address wraps past the top of the address space
   only to below 0x28, which user_range_valid refuses. A failed call
   reports 0 in every result, except where syscall_reports_results says
   otherwise (S7). R10 comes back zero (S4). Interrupts stay disabled
   from the termination check to SYSRETQ, which syscall.S reaches with
   them disabled, so a termination that begins later finds the thread in
   user mode, and the return from the next interrupt stops it. */
void syscall_dispatch(INTERRUPT_FRAME *frame)
{
    UINT64 arguments[SYSCALL_ARGUMENTS_MAX];
    UINT64 results[SYSCALL_RESULTS];
    UINT32 count = syscall_argument_count(frame->rax);
    STATUS status = OK;
    UINT32 i;

    arguments[0] = frame->r10;
    arguments[1] = frame->rdx;
    arguments[2] = frame->r8;
    arguments[3] = frame->r9;
    for (i = 0; i < SYSCALL_RESULTS; i++)
        results[i] = 0;
    if (count > REGISTER_ARGUMENTS)
        status = copy_from_user(&arguments[REGISTER_ARGUMENTS],
                                frame->rsp + STACK_ARGUMENTS_OFFSET,
                                (count - REGISTER_ARGUMENTS) * sizeof(UINT64));
    if (status == OK)
        status = syscall_invoke(frame->rax, arguments, results);
    if (status != OK && !syscall_reports_results(frame->rax, status))
        for (i = 0; i < SYSCALL_RESULTS; i++)
            results[i] = 0;
    frame->rax = status;
    frame->rdx = results[0];
    frame->r8 = results[1];
    frame->r9 = results[2];
    frame->r10 = 0;
    disable_interrupts();
    process_user_return();
}

/* A 32-bit thread enters by INT (S11). Intel processors do not support
   SYSCALL in compatibility mode (SDM Vol. 3A 6.8.8), and AMD's enter
   here (APM 6.1.1), so the process ends as for an invalid opcode, which
   keeps the outcome one on both. */
_Noreturn void syscall_compat(void)
{
    serial_write("nova: syscall from compatibility mode, process ended\r\n");
    process_fault(INVALID_OPCODE_VECTOR);
}

/* The 32-bit lane (S11). Code in compatibility mode sees only the low 32
   bits of a general register, and the upper 32 are undefined once it
   has run (SDM Vol. 1 3.4.1.1), so the call number and the argument
   array's address are read from EAX and EDX alone, and the results are
   zero-extended. A process not created 32-bit ends as an INT to a gate
   of DPL 0 would end it, with #GP (SDM Vol. 3A 7.12.1.2): whatever mode
   its code reaches, the lane is not its interface. The interrupt gate
   left IF clear; the call runs with it set, as SYSCALL's do, and the
   return through return_to_user needs it clear again (entry.S). */
void compat_syscall_dispatch(INTERRUPT_FRAME *frame)
{
    UINT64 arguments[SYSCALL_ARGUMENTS_MAX];
    UINT64 results[SYSCALL_RESULTS];
    UINT32 number = (UINT32)frame->rax;
    UINT32 count = syscall_argument_count(number);
    STATUS status = OK;
    UINT32 i;

    if (!process_is_32_bit(process_current())) {
        serial_write("nova: 32-bit gate entered by a 64-bit process, ended\r\n");
        process_fault(GENERAL_PROTECTION_VECTOR);
    }
    enable_interrupts();
    for (i = 0; i < SYSCALL_RESULTS; i++)
        results[i] = 0;
    if (!syscall_served_32_bit(number))
        status = ERR_NOT_SUPPORTED;
    else if (count)
        status = copy_from_user(arguments, (UINT32)frame->rdx,
                                count * sizeof(UINT64));
    if (status == OK)
        status = syscall_invoke(number, arguments, results);
    if (status != OK && !syscall_reports_results(number, status))
        for (i = 0; i < SYSCALL_RESULTS; i++)
            results[i] = 0;
    disable_interrupts();
    frame->rax = status;
    frame->rdx = (UINT32)results[0];
    frame->rcx = (UINT32)results[1];
}

/* ------------------------------------------------------------------------
   Faults. */

/* A fault in user mode is the process's, never the kernel's: the process
   ends with the vector as its exit code (docs/spec/objects.md O10). */
_Noreturn void user_exception(const INTERRUPT_FRAME *frame)
{
    serial_write("nova: user exception ");
    serial_number(frame->vector);
    serial_write(" at rip ");
    serial_hex(frame->rip);
    if (has_error_code(frame->vector)) {
        serial_write(", error code ");
        serial_hex(frame->error_code);
    }
    if (frame->vector == PAGE_FAULT_VECTOR) {
        serial_write(", cr2 ");
        serial_hex(read_cr2());
    }
    serial_write("\r\n");
    process_fault((UINT32)frame->vector);
}

/* S5: the return the exit path refused would have raised #GP. */
_Noreturn void user_bad_return(const INTERRUPT_FRAME *frame)
{
    serial_write("nova: return to user rip ");
    serial_hex(frame->rip);
    serial_write(" above user memory refused\r\n");
    process_fault(GENERAL_PROTECTION_VECTOR);
}

/* ------------------------------------------------------------------------
   User memory. */

UINT64 user_space_top(void)
{
    return user_top;
}

int user_range_valid(UINT64 address, UINT64 size)
{
    return address >= USER_BASE && address <= user_top &&
           size <= user_top - address;
}

/* user_copy's other operand is a kernel buffer, and a fault on that is
   the kernel's own. */
UINT64 user_copy_fixup(UINT64 rip, UINT64 address)
{
    return rip == (UINT64)user_copy_access && address < user_top
               ? (UINT64)user_copy_fault
               : 0;
}

STATUS copy_from_user(void *to, UINT64 from, UINT64 size)
{
    if (!user_range_valid(from, size))
        return ERR_BAD_ADDRESS;
    return size ? user_copy(to, (const void *)from, size) : OK;
}

STATUS copy_to_user(UINT64 to, const void *from, UINT64 size)
{
    if (!user_range_valid(to, size))
        return ERR_BAD_ADDRESS;
    return size ? user_copy((void *)to, from, size) : OK;
}
