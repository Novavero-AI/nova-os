/* The x87, SSE and AVX state of user threads. The kernel enables x87,
   SSE and, where the processor has it, AVX state, and nothing beyond
   (docs/spec/syscalls.md S4), so AVX-512, AMX and APX registers do not
   exist for user code. It saves a thread's state with XSAVE where the
   processor has the XSAVE feature set and with FXSAVE where it does not;
   QEMU's default processor model has no XSAVE. */

#include "fpu.h"
#include "cpu.h"
#include "serial.h"

/* CR0 and CR4 bits for x87 and SSE (SDM Vol. 3A 2.5, 16.1.3): MP and NE
   set, so that WAIT obeys TS and x87 errors raise #MF; EM and TS clear,
   so that no x87 or SSE instruction faults; OSFXSR and OSXMMEXCPT set,
   so that SSE instructions run and SIMD floating-point errors raise #XM;
   OSXSAVE enables the XSAVE feature set (SDM Vol. 1 13.3). */
#define CR0_MONITOR_COPROCESSOR (1ULL << 1)
#define CR0_EMULATION (1ULL << 2)
#define CR0_TASK_SWITCHED (1ULL << 3)
#define CR0_NUMERIC_ERROR (1ULL << 5)
#define CR4_OSFXSR (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)
#define CR4_OSXSAVE (1ULL << 18)

/* CPUID.01H:ECX.XSAVE[26] and .AVX[28] (SDM Vol. 1 13.2, 14.3);
   CPUID.0DH.00H:EAX gives the XCR0 bits the processor supports and EBX
   the size of the XSAVE area for the bits XCR0 has set (SDM Vol. 1
   13.2). */
#define CPUID_FEATURES 1
#define CPUID_FEATURES_ECX_XSAVE (1u << 26)
#define CPUID_FEATURES_ECX_AVX (1u << 28)
#define CPUID_XSAVE 0xd

/* XCR0's state components (SDM Vol. 1 13.1, 13.3). */
#define XCR0_X87 0x1ULL
#define XCR0_SSE 0x2ULL
#define XCR0_AVX 0x4ULL

/* The FXSAVE area is 512 bytes (SDM Vol. 1 10.5.1), the legacy region of
   an XSAVE area the same 512 bytes (SDM Vol. 1 13.4.1): the x87 control
   word at 0, MXCSR at 24. XSTATE_BV, the components the area holds, is
   the first 8 bytes of the XSAVE header at 512 (SDM Vol. 1 13.4.2). */
#define FXSAVE_AREA_SIZE 512
#define AREA_FCW 0
#define AREA_MXCSR 24
#define AREA_XSTATE_BV 512

/* The x87 control word and MXCSR a thread starts with (S8): all
   exceptions masked, round to nearest, and double precision for the
   x87, the values the x64 calling convention gives a program's start
   ("FPCSR", "MXCSR"). */
#define START_FCW 0x027f
#define START_MXCSR 0x1f80

/* XSAVE64, XRSTOR64 and XSETBV with the area's address in RCX (SDM Vol.
   2C, XSAVE: REX.W 0F AE /4; XRSTOR: REX.W 0F AE /5; XSETBV: 0F 01 D1),
   which TinyCC's assembler lacks; and FXSAVE64 and FXRSTOR64 (REX.W 0F
   AE /0 and /1), written the same way for clang's assembler. */
#define XSAVE64_RCX ".byte 0x48, 0x0f, 0xae, 0x21"
#define XRSTOR64_RCX ".byte 0x48, 0x0f, 0xae, 0x29"
#define FXSAVE64_RCX ".byte 0x48, 0x0f, 0xae, 0x01"
#define FXRSTOR64_RCX ".byte 0x48, 0x0f, 0xae, 0x09"
#define XSETBV ".byte 0x0f, 0x01, 0xd1"

/* The XCR0 bits in use and the area size, or 0 and FXSAVE_AREA_SIZE
   where the processor has no XSAVE. */
static UINT64 xsave_components;
static UINT64 area_size;

static void xsetbv(UINT32 index, UINT64 value)
{
    __asm__ volatile(XSETBV
                     :
                     : "c"(index), "a"((UINT32)value),
                       "d"((UINT32)(value >> 32))
                     : "memory");
}

void fpu_init(UINT64 max_size)
{
    UINT32 a, b, c, d;

    write_cr0((read_cr0() & ~(CR0_EMULATION | CR0_TASK_SWITCHED)) |
              CR0_MONITOR_COPROCESSOR | CR0_NUMERIC_ERROR);
    write_cr4(read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);
    area_size = FXSAVE_AREA_SIZE;
    cpuid(CPUID_FEATURES, &a, &b, &c, &d);
    if (c & CPUID_FEATURES_ECX_XSAVE) {
        UINT32 avx = c & CPUID_FEATURES_ECX_AVX;

        write_cr4(read_cr4() | CR4_OSXSAVE);
        cpuid(CPUID_XSAVE, &a, &b, &c, &d);
        xsave_components = XCR0_X87 | XCR0_SSE;
        if (avx && (a & XCR0_AVX))
            xsave_components |= XCR0_AVX;
        xsetbv(0, xsave_components);
        cpuid(CPUID_XSAVE, &a, &b, &c, &d);
        area_size = b;
    }
    if (area_size > max_size) {
        serial_write("nova: fpu refused bytes=");
        serial_number(area_size);
        serial_write(" max_bytes=");
        serial_number(max_size);
        serial_write("\r\n");
        serial_halt();
    }
    serial_write("nova: fpu save=");
    if (xsave_components)
        serial_write(xsave_components & XCR0_AVX ? "xsave state=x87,sse,avx"
                                                 : "xsave state=x87,sse");
    else
        serial_write("fxsave state=x87,sse");
    serial_write(" bytes=");
    serial_number(area_size);
    serial_write("\r\n");
}

UINT64 fpu_area_size(void)
{
    return area_size;
}

/* With XSAVE, XSTATE_BV names only the x87 state as present, so XRSTOR
   loads the x87 registers from the area and gives SSE and AVX their
   initial configuration, every register zero; it loads MXCSR from the
   area whenever it restores SSE or AVX state at all (SDM Vol. 1 13.8.1).
   The rest of the header must be zero (same section). An all-zero
   abridged tag word marks every x87 register empty (SDM Vol. 1
   10.5.1.1). */
void fpu_state_init(UINT8 *area)
{
    UINT64 i;

    for (i = 0; i < area_size; i++)
        area[i] = 0;
    *(UINT16 *)(area + AREA_FCW) = START_FCW;
    *(UINT32 *)(area + AREA_MXCSR) = START_MXCSR;
    if (xsave_components)
        *(UINT64 *)(area + AREA_XSTATE_BV) = XCR0_X87;
}

/* EDX:EAX all ones asks for every component XCR0 enables (SDM Vol. 1
   13.7). */
void fpu_save(UINT8 *area)
{
    if (xsave_components)
        __asm__ volatile(XSAVE64_RCX
                         :
                         : "c"(area), "a"(0xffffffffu), "d"(0xffffffffu)
                         : "memory");
    else
        __asm__ volatile(FXSAVE64_RCX : : "c"(area) : "memory");
}

void fpu_restore(const UINT8 *area)
{
    if (xsave_components)
        __asm__ volatile(XRSTOR64_RCX
                         :
                         : "c"(area), "a"(0xffffffffu), "d"(0xffffffffu)
                         : "memory");
    else
        __asm__ volatile(FXRSTOR64_RCX : : "c"(area) : "memory");
}
