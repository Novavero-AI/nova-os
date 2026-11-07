/* The kernel's interrupt descriptor table and the C side of interrupt
   entry. Gates lead to the stubs in entry.S, which save the interrupted
   state and call interrupt_dispatch. */

#include "x86_64.h"
#include "cpu.h"
#include "serial.h"
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
   Table 3-2), which clears IF on entry (SDM Vol. 3A 7.12.1.3). */
#define GATE_PRESENT 0x80
#define GATE_INTERRUPT 0x0e

/* Vectors without a handler keep an all-zero gate. Its type is not an
   interrupt or trap gate, so delivering one raises #GP with an error code
   naming the vector (SDM Vol. 2A, INT n, IA-32e mode; Vol. 3A 7.13). */
static IDT_GATE idt[VECTOR_COUNT];

/* ist 0 keeps the interrupted stack; 1 to 7 switch to that IST slot's
   stack (SDM Vol. 3A 7.14.5). */
static void set_gate(UINT32 vector, UINT64 handler, UINT8 ist)
{
    IDT_GATE *gate = &idt[vector];

    gate->offset_low = (UINT16)handler;
    gate->selector = KERNEL_CODE_SELECTOR;
    gate->ist = ist;
    gate->type = GATE_PRESENT | GATE_INTERRUPT;
    gate->offset_middle = (UINT16)(handler >> 16);
    gate->offset_high = (UINT32)(handler >> 32);
}

void idt_load(void)
{
    DESCRIPTOR_REGISTER idtr;
    UINT32 vector;

    for (vector = 0; vector < EXCEPTION_COUNT; vector++)
        set_gate(vector, exception_stubs[vector], 0);
    set_gate(DOUBLE_FAULT_VECTOR, exception_stubs[DOUBLE_FAULT_VECTOR],
             DOUBLE_FAULT_IST);
    set_gate(PAGE_FAULT_VECTOR, exception_stubs[PAGE_FAULT_VECTOR],
             PAGE_FAULT_IST);
    set_gate(TIMER_VECTOR, (UINT64)timer_stub, 0);
    set_gate(SPURIOUS_VECTOR, (UINT64)spurious_stub, 0);
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (UINT64)idt;
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}

static int has_error_code(UINT64 vector)
{
    return vector < EXCEPTION_COUNT && (ERROR_CODE_VECTORS >> vector & 1);
}

static void log_breakpoint(const INTERRUPT_FRAME *frame)
{
    serial_write("nova: breakpoint caught at rip ");
    serial_hex(frame->rip);
    serial_write("\r\n");
}

/* The kernel has no recovery from an exception yet: report it and stop. */
static void exception_halt(const INTERRUPT_FRAME *frame)
{
    serial_write("nova: exception ");
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
    serial_write(", rsp ");
    serial_hex(frame->rsp);
    serial_write(", halted\r\n");
    halt_forever();
}

void interrupt_dispatch(const INTERRUPT_FRAME *frame)
{
    switch (frame->vector) {
    case BREAKPOINT_VECTOR:
        log_breakpoint(frame);
        break;
    case TIMER_VECTOR:
        apic_timer_interrupt();
        sched_timer_interrupt();
        break;
    case SPURIOUS_VECTOR:
        /* Never set in the in-service register, so it takes no EOI (SDM
           Vol. 3A 13.9). */
        break;
    default:
        exception_halt(frame);
    }
}
