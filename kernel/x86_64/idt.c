/* The kernel's interrupt descriptor table and the C side of interrupt
   entry. Gates lead to the stubs in entry.S, which save the interrupted
   state and call interrupt_dispatch, or exception_halt for the three
   events that have stacks of their own. */

#include "x86_64.h"
#include "cpu.h"
#include "serial.h"
#include "../object/interrupt.h"
#include "../object/process.h"
#include "../sched/thread.h"

#define VECTOR_COUNT 256

/* A 64-bit mode interrupt gate (SDM Vol. 3A 7.14.1, Figure 7-8). */
typedef struct {
    UINT16 offset_low;
    UINT16 selector;
    UINT8 ist;
    UINT8 type;
    UINT16 offset_middle;
    UINT32 offset_high;
    UINT32 reserved;
} IDT_GATE;

_Static_assert(sizeof(IDT_GATE) == 16, "an IDT gate is 16 bytes");

/* Present, DPL 0, type 1110b: a 64-bit interrupt gate (SDM Vol. 3A
   Table 3-2), which clears IF on entry (SDM Vol. 3A 7.12.1.3). With DPL
   0, user code cannot raise any vector with INT n, INT3 or INTO: the
   processor raises #GP instead (SDM Vol. 3A 7.12.1.2). The 32-bit lane's
   gate alone has DPL 3, so that INT n reaches it from CPL 3; the
   processor checks a gate's DPL only for INT n, INT3 and INTO, so it
   changes nothing for other events (7.12.1.2). An interrupt gate rather
   than a trap gate: IF stays clear until the entry has swapped GS
   (entry.S), and the dispatch sets it once the call is under way. */
#define GATE_PRESENT 0x80
#define GATE_DPL_USER 0x60
#define GATE_INTERRUPT 0x0e

/* Vectors without a handler keep an all-zero gate. Its type is not an
   interrupt or trap gate, so delivering one raises #GP with an error code
   naming the vector (SDM Vol. 2A, INT n, IA-32e mode; Vol. 3A 7.13). */
static IDT_GATE idt[VECTOR_COUNT];

_Static_assert(MSI_VECTOR_FIRST > TIMER_VECTOR &&
                   MSI_VECTOR_FIRST + MSI_VECTOR_COUNT <= COMPAT_GATE_VECTOR,
               "the message-signalled vectors take no vector of another use");

/* ist 0 keeps the interrupted stack, or takes RSP0 from the TSS on a
   change to CPL 0; 1 to 7 switch to that IST slot's stack (SDM Vol. 3A
   7.14.4, 7.14.5). */
static void set_gate(UINT32 vector, UINT64 handler, UINT8 ist, UINT8 dpl)
{
    IDT_GATE *gate = &idt[vector];

    gate->offset_low = (UINT16)handler;
    gate->selector = KERNEL_CODE_SELECTOR;
    gate->ist = ist;
    gate->type = GATE_PRESENT | dpl | GATE_INTERRUPT;
    gate->offset_middle = (UINT16)(handler >> 16);
    gate->offset_high = (UINT32)(handler >> 32);
}

void idt_load(void)
{
    DESCRIPTOR_REGISTER idtr;
    UINT32 vector;

    for (vector = 0; vector < EXCEPTION_COUNT; vector++)
        set_gate(vector, exception_stubs[vector], 0, 0);
    set_gate(NMI_VECTOR, exception_stubs[NMI_VECTOR], NMI_IST, 0);
    set_gate(DOUBLE_FAULT_VECTOR, exception_stubs[DOUBLE_FAULT_VECTOR],
             DOUBLE_FAULT_IST, 0);
    set_gate(MACHINE_CHECK_VECTOR, exception_stubs[MACHINE_CHECK_VECTOR],
             MACHINE_CHECK_IST, 0);
    set_gate(TIMER_VECTOR, (UINT64)timer_stub, 0, 0);
    set_gate(SPURIOUS_VECTOR, (UINT64)spurious_stub, 0, 0);
    set_gate(COMPAT_GATE_VECTOR, (UINT64)compat_gate_stub, 0, GATE_DPL_USER);
    for (vector = 0; vector < MSI_VECTOR_COUNT; vector++)
        set_gate(MSI_VECTOR_FIRST + vector,
                 (UINT64)msi_stubs + vector * MSI_STUB_SIZE, 0, 0);
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (UINT64)idt;
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}

static void log_breakpoint(const INTERRUPT_FRAME *frame)
{
    serial_write("nova: breakpoint caught at rip ");
    serial_hex(frame->rip);
    serial_write("\r\n");
}

/* The kernel has no recovery from an exception of its own: report it and
   stop. A stack overflow faults on the stack's guard page, and the page
   fault cannot be delivered on the exhausted stack, so it ends here as a
   double fault (SDM Vol. 3A 7.15, Interrupt 8), with the guard's address
   in CR2. */
_Noreturn void exception_halt(const INTERRUPT_FRAME *frame)
{
    serial_write("nova: exception ");
    serial_number(frame->vector);
    serial_write(" at rip ");
    serial_hex(frame->rip);
    if (has_error_code(frame->vector)) {
        serial_write(", error code ");
        serial_hex(frame->error_code);
    }
    if (frame->vector == PAGE_FAULT_VECTOR ||
        frame->vector == DOUBLE_FAULT_VECTOR) {
        serial_write(", cr2 ");
        serial_hex(read_cr2());
    }
    serial_write(", rsp ");
    serial_hex(frame->rsp);
    serial_write(", halted\r\n");
    halt_forever();
}

/* A page fault in the kernel is a bug, unless it is user_copy's access
   to user memory, which then fails instead (docs/spec/syscalls.md S6). */
static void kernel_page_fault(INTERRUPT_FRAME *frame)
{
    UINT64 fixup = user_copy_fixup(frame->rip, read_cr2());

    if (!fixup)
        exception_halt(frame);
    frame->rip = fixup;
}

/* An interrupt from user mode returns there from here with interrupts
   disabled until IRETQ, so a thread whose process is terminating, stopped
   while it ran or while the scheduler had switched away from it here,
   exits instead of returning. */
void interrupt_dispatch(INTERRUPT_FRAME *frame)
{
    if (frame->vector < EXCEPTION_COUNT && (frame->cs & SELECTOR_RPL_MASK))
        user_exception(frame);
    switch (frame->vector) {
    case BREAKPOINT_VECTOR:
        log_breakpoint(frame);
        break;
    case PAGE_FAULT_VECTOR:
        kernel_page_fault(frame);
        break;
    case TIMER_VECTOR:
        apic_timer_interrupt();
        sched_timer_interrupt();
        break;
    case SPURIOUS_VECTOR:
        /* Never set in the in-service register, so it takes no EOI (SDM
           Vol. 3A 13.9). */
        break;
    case COMPAT_GATE_VECTOR:
        /* The kernel never executes INT, so an entry from kernel mode is
           a bug. */
        if (!(frame->cs & SELECTOR_RPL_MASK))
            exception_halt(frame);
        compat_syscall_dispatch(frame);
        break;
    default:
        if (frame->vector >= MSI_VECTOR_FIRST &&
            frame->vector - MSI_VECTOR_FIRST < MSI_VECTOR_COUNT)
            interrupt_fired((UINT32)frame->vector);
        else
            exception_halt(frame);
    }
    if (frame->cs & SELECTOR_RPL_MASK)
        process_user_return();
}
