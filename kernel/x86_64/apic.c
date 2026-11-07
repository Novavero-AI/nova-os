/* The Local APIC as the kernel's interrupt controller and clock. The
   legacy 8259 PICs are masked, the APIC is enabled, and its timer is
   calibrated against the timestamp counter and run periodically at
   TIMER_HZ. Registers and bits are from the SDM, Vol. 3A chapter 13, and
   agree with EDK2's LocalApic.h and QEMU's hw/intc/apic.c. */

#include "arch.h"
#include "x86_64.h"
#include "cpu.h"
#include "paging.h"
#include "serial.h"

/* The 8259 pair's interrupt mask registers (EDK2's 8259.h); a write to
   the data port outside the initialization sequence sets the mask (QEMU's
   hw/intc/i8259.c). */
#define PIC_MASTER_MASK 0x21
#define PIC_SLAVE_MASK 0xa1
#define PIC_MASK_ALL 0xff

/* IA32_APIC_BASE (SDM Vol. 3A 13.4.4, and Table 13-5 for the two mode
   bits). The base address is the MSR with bits 11:0 cleared, as EDK2's
   GetLocalApicBaseAddress takes it. */
#define IA32_APIC_BASE 0x1b
#define APIC_BASE_X2APIC (1ULL << 10)
#define APIC_BASE_ENABLE (1ULL << 11)
#define APIC_BASE_ADDRESS (~0xfffULL)

/* Register offsets from the APIC's base (SDM Vol. 3A, Table 13-1). */
#define APIC_ID 0x20
#define APIC_TPR 0x80
#define APIC_EOI 0xb0
#define APIC_SVR 0xf0
#define APIC_LVT_TIMER 0x320
#define APIC_TIMER_INITIAL 0x380
#define APIC_TIMER_CURRENT 0x390
#define APIC_TIMER_DIVIDE 0x3e0

/* Spurious-interrupt vector register (SDM Vol. 3A, Figure 13-23). */
#define SVR_VECTOR 0xff
#define SVR_SOFTWARE_ENABLE (1u << 8)

/* LVT timer register (SDM Vol. 3A, Figure 13-8): timer mode in bits
   18:17, where 00b is one-shot and 01b periodic. */
#define LVT_MASKED (1u << 16)
#define LVT_TIMER_PERIODIC (1u << 17)

/* Divide value 111b, in bits 0, 1 and 3 (SDM Vol. 3A, Figure 13-10). */
#define TIMER_DIVIDE_BY_1 0xb
#define TIMER_COUNT_MAX 0xffffffffu

#define CALIBRATION_MS 10
#define MS_PER_SECOND 1000

/* The local APIC ID is in bits 31:24 of its register (SDM Vol. 3A
   13.4.6, Figure 13-6). An MSI's address is 0FEEH in bits 31:20 and the
   destination's APIC ID in bits 19:12, with redirection hint and
   destination mode 0 for that one processor; its data is the vector in
   bits 7:0, with fixed delivery (000B in bits 10:8) and edge trigger
   (bit 15 clear) (SDM Vol. 3A 13.11.1, 13.11.2). */
#define APIC_ID_SHIFT 24
#define MSI_ADDRESS_BASE 0xfee00000u
#define MSI_DESTINATION_SHIFT 12

/* Where the kernel reaches the register block: its direct-map address,
   mapped strong uncacheable, which correct APIC operation requires (SDM
   Vol. 3A 13.4.1). */
static UINT64 apic_base;

/* Written by the timer interrupt, read by the code it interrupts. */
static volatile UINT64 ticks;

static UINT32 apic_read(UINT32 reg)
{
    return *(volatile UINT32 *)(apic_base + reg);
}

static void apic_write(UINT32 reg, UINT32 value)
{
    *(volatile UINT32 *)(apic_base + reg) = value;
}

/* The PICs reach the processor through the APIC's LINT0 pin as ExtINT
   (SDM Vol. 3A 13.5.1); with every line masked they raise nothing. */
static void pic_mask_all(void)
{
    outb(PIC_MASTER_MASK, PIC_MASK_ALL);
    outb(PIC_SLAVE_MASK, PIC_MASK_ALL);
    serial_write("nova: 8259 pics masked\r\n");
}

/* The registers are memory-mapped only in xAPIC mode: globally enabled,
   x2APIC mode off (SDM Vol. 3A 13.12). Firmware that hands the APIC over
   in another mode stops the kernel here. */
static void apic_enable(void)
{
    UINT64 msr = rdmsr(IA32_APIC_BASE);

    if ((msr & (APIC_BASE_ENABLE | APIC_BASE_X2APIC)) != APIC_BASE_ENABLE) {
        serial_write("nova: local apic not in xapic mode, IA32_APIC_BASE ");
        serial_hex(msr);
        serial_write(", halted\r\n");
        halt_forever();
    }
    apic_base = paging_map_uncached(msr & APIC_BASE_ADDRESS);
    if (!apic_base) {
        serial_write("nova: cannot map the local apic uncached, halted\r\n");
        halt_forever();
    }
    /* Task priority 0 holds back no vector the APIC can deliver (SDM
       Vol. 3A 13.8.3.1). */
    apic_write(APIC_TPR, 0);
    apic_write(APIC_SVR, (apic_read(APIC_SVR) & ~SVR_VECTOR) |
                             SVR_SOFTWARE_ENABLE | SPURIOUS_VECTOR);
    serial_write("nova: local apic at ");
    serial_hex(msr & APIC_BASE_ADDRESS);
    serial_write(" mapped uncached at ");
    serial_hex(apic_base);
    serial_write(", enabled\r\n");
}

/* The timer's rate at TIMER_DIVIDE_BY_1, from a masked one-shot count
   over CALIBRATION_MS of the timestamp counter. */
static UINT64 timer_ticks_per_ms(UINT64 tsc_per_ms)
{
    UINT64 start;

    apic_write(APIC_LVT_TIMER, LVT_MASKED | TIMER_VECTOR);
    apic_write(APIC_TIMER_DIVIDE, TIMER_DIVIDE_BY_1);
    apic_write(APIC_TIMER_INITIAL, TIMER_COUNT_MAX);
    start = rdtsc();
    while (rdtsc() - start < CALIBRATION_MS * tsc_per_ms)
        ;
    return (TIMER_COUNT_MAX - apic_read(APIC_TIMER_CURRENT)) / CALIBRATION_MS;
}

void apic_timer_start(UINT64 tsc_per_ms)
{
    UINT64 per_ms;

    pic_mask_all();
    apic_enable();
    per_ms = timer_ticks_per_ms(tsc_per_ms);
    /* Changing the mode does not start the timer; writing the initial
       count does, restarting the calibration count (SDM Vol. 3A 13.5.4). */
    apic_write(APIC_LVT_TIMER, LVT_TIMER_PERIODIC | TIMER_VECTOR);
    apic_write(APIC_TIMER_INITIAL,
               (UINT32)(per_ms * MS_PER_SECOND / TIMER_HZ));
    serial_write("nova: apic timer ");
    serial_number(per_ms);
    serial_write(" ticks per ms, ");
    serial_number(TIMER_HZ);
    serial_write(" Hz\r\n");
}

void apic_timer_interrupt(void)
{
    ticks++;
    apic_eoi();
}

void apic_eoi(void)
{
    apic_write(APIC_EOI, 0);
}

void apic_msi_message(UINT32 vector, UINT64 *address, UINT32 *data)
{
    UINT32 id = apic_read(APIC_ID) >> APIC_ID_SHIFT;

    *address = MSI_ADDRESS_BASE | id << MSI_DESTINATION_SHIFT;
    *data = vector;
}

UINT64 timer_ticks(void)
{
    return ticks;
}

void timer_sleep_until(UINT64 tick)
{
    disable_interrupts();
    while (ticks < tick)
        wait_for_interrupt();
    enable_interrupts();
}
