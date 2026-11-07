# System calls (x86-64)

How user code enters the nova-os kernel on x86-64, which registers carry
what, how errors are reported, and the first set of calls. The objects and
rights the calls act on are specified in [objects.md](objects.md), and
channels, ports and the driver calls in [ipc.md](ipc.md).

Specifications are cited by the versions in [README.md](README.md): "SDM"
is the Intel manual's Volume 3, "APM" the AMD manual's Volume 2, "x64
calling convention" and "x64 stack usage" the Microsoft pages pinned there.
"seL4 10.1" is section 10.1 of the seL4 Reference Manual and "Liedtke
1993" the paper, at the versions pinned under
[Design references](README.md#design-references). Other pages are linked
under [References](#references).

## S1. Entry mechanism

**Decided.**

User code enters the kernel with `SYSCALL` and the kernel returns with the
64-bit form of `SYSRET`. The kernel checks at boot that the processor has
them (CPUID.80000001H:EDX[11], SDM 6.8.8).

- **Why:** `SYSCALL` and `SYSRET` exist in 64-bit mode on both vendors
  (SDM 6.8.8, APM 6.1.1) and load fixed segment values from MSRs instead
  of reading descriptors, which is what makes them fast (APM 6.1.1).
  `SYSENTER` and `SYSEXIT` are no alternative: on AMD processors they "are
  illegal in long mode and result in an undefined opcode exception" (APM
  6.1.2).
- **Cost:** `SYSCALL` neither switches stacks nor saves the stack pointer
  (SDM 6.8.8), so the kernel does both in software, which creates the
  windows S3 and S5 close.
- **Not taken:** `INT n` through an IDT gate user code may call. The
  processor switches to the kernel stack from the TSS and saves the user's
  stack pointer, flags and return address itself (SDM 7.12.1, 7.14.4), so
  no window exists in which the kernel runs on a user stack. Its cost is a
  gate-descriptor and segment-descriptor load on every call, work `SYSCALL`
  replaces with MSR values.
- **Later:** FRED. Where CPUID.07H.01H:EAX.FRED[17] is 1 (SDM 8.2.1), FRED
  event delivery replaces `SYSCALL`'s entry, `SYSRET` raises #UD, and
  `ERETU` returns to user mode (SDM 6.8.8, 8.5.4). The register convention
  in S4 does not depend on the instruction, so moving to FRED changes no
  user code.

## S2. MSRs and descriptor tables

**Decided.**

| MSR | Address | Value |
|---|---|---|
| `IA32_EFER` | C000_0080h | `SCE` (bit 0) set, other bits kept (SDM 2.2.1, APM 3.1.7) |
| `IA32_STAR` | C000_0081h | 0x001B_0008_0000_0000: `SYSRET` selector base 0x1B in bits 63:48, `SYSCALL` selector 0x08 in bits 47:32, bits 31:0 zero |
| `IA32_LSTAR` | C000_0082h | The kernel's system-call entry |
| `CSTAR` | C000_0083h | An entry that ends the calling process as an invalid opcode would |
| `IA32_FMASK` | C000_0084h | 0x0004_4700: clears TF (8), IF (9), DF (10), NT (14) and AC (18) on entry |
| `IA32_KERNEL_GS_BASE` | C000_0102h | The CPU's per-CPU block while user code runs |

The addresses of the four `SYSCALL` MSRs are the APM's (APM 6.1.1,
Figure 6-1, and 6.1.3); the SDM names them `IA32_STAR`, `IA32_LSTAR` and
`IA32_FMASK` (SDM 2.1.6.1) and gives their addresses in Volume 4, which is
not pinned. `IA32_KERNEL_GS_BASE` is at C0000102H in both (SDM 2.8.2).

The GDT grows by three entries after the kernel's existing two
(`kernel/x86_64/gdt.c`):

| Selector | Descriptor |
|---|---|
| 0x00 | Null |
| 0x08 | Kernel code, 64-bit (unchanged) |
| 0x10 | Kernel data (unchanged) |
| 0x18 | User code, 32-bit (L clear, D set), DPL 3; loaded as 0x1B by the 32-bit lane (S11) |
| 0x20 | User data, DPL 3; loaded as 0x23 |
| 0x28 | User code, 64-bit, DPL 3; loaded as 0x2B |
| 0x30 | The CPU's 64-bit TSS, a 16-byte descriptor (SDM 10.2.3) |

`SYSCALL` loads CS from `STAR[47:32]` = 0x08 and SS from that plus 8,
0x10. A 64-bit `SYSRET` loads CS from `STAR[63:48]` + 16 = 0x2B and SS
from `STAR[63:48]` + 8 = 0x23 (SDM 6.8.8). The selector bases carry the
privilege levels the APM asks for: RPL 3 in bits 49:48 and RPL 0 in bits
33:32 (APM 6.1.1).

Each CPU's TSS holds `RSP0`, the running thread's kernel stack top, which
the processor loads on an interrupt from user mode; three IST stacks: IST1
for NMI, IST2 for double fault, IST3 for machine check (SDM 7.14.5); and
room for the I/O permission bitmap ([ipc.md I6](ipc.md#i6-device-memory-and-io-ports)).
The kernel stores a pointer to the CPU's per-CPU block at the top of each
IST stack (S3).

- **Why `CSTAR` ends the process:** `CSTAR` is the entry for `SYSCALL`
  from compatibility mode (APM 6.1.1), which Intel processors do not
  support (SDM 6.8.8). 32-bit code enters by the gate of S11, so the
  entry ends the process as the #UD an Intel processor raises would,
  and the outcome is one on both vendors. The 32-bit code descriptor at
  0x18 is the one a 32-bit `SYSRET` would load; the kernel executes only
  the 64-bit form.
- **Why these flags:** IF clear keeps maskable interrupts from arriving
  while the kernel is still on the user's stack, since a handler at the
  same privilege level runs on the current stack (SDM 7.12.1). TF clear
  keeps a user's single-step trap out of the entry path. DF clear is what
  the kernel's C code assumes. NT clear keeps a later `IRETQ` from faulting,
  as `IRET` with NT set does in 64-bit mode (SDM 10.7). AC clear keeps
  supervisor-mode access prevention in force (S6).
- **Cost:** the user segments must sit in this order, since `STAR`'s
  arithmetic fixes their selectors, and each CPU needs its own TSS and IST
  stacks.
- **Not taken:** an IST stack for #GP, the third remedy SDM 6.8.8 lists
  for the `SYSRET` hazard. An IST entry loads the same stack pointer every
  time (SDM 7.14.5), so a #GP raised inside the #GP handler would
  overwrite the first one's frame; S5 avoids the hazard instead.

## S3. Entry and exit

**Decided.**

Entry, at `IA32_LSTAR`:

1. The processor saves the return address in RCX and RFLAGS in R11,
   clears the `IA32_FMASK` flags, loads the kernel's CS and SS, and jumps
   (SDM 6.8.8). RSP is still the user's.
2. `SWAPGS`: GS now addresses the per-CPU block (APM 6.1.3, SDM 2.8.2).
3. Store the user's RSP in the per-CPU block and load the running
   thread's kernel stack top from it.
4. Push the system-call frame: the user's RSP, RCX, R11 and the
   registers S4 says the kernel preserves.
5. `STI`.
6. If RAX is not a call number in the table (S8), the status is
   `ERR_NOT_SUPPORTED`; otherwise call the handler. The kernel's C code is
   compiled for the Microsoft x64 convention, so the handler takes
   arguments 1 to 4 in RCX, RDX, R8 and R9 and the rest on its stack. For
   a call with more than four arguments, the entry first copies arguments
   5 to 8 from the user stack with the user-memory routines, after
   checking that the range from `RSP+0x28` lies in user memory (S6); if it
   does not, or the copy faults, the call fails with `ERR_BAD_ADDRESS`.

Exit:

1. `CLI`.
2. Set RAX to the status and RDX, R8 and R9 to the results or zero; set
   R10 to zero; restore the preserved registers.
3. Check the return address (S5).
4. Load RCX with the return address and R11 with the return flags (S5).
5. Load the user's RSP.
6. `SWAPGS`.
7. `SYSRETQ`.

A thread that entered by `SYSCALL` leaves by `SYSRET`. A thread that
entered by an interrupt or exception, or is starting for the first time,
leaves by `IRETQ`, which restores RCX and R11 exactly.

Entry steps 1 to 3 and exit steps 5 to 7 are the windows in which the
processor runs at CPL 0 with the user's stack pointer or GS base. IF is
clear in both, as SDM 6.8.8 prescribes for the exit, so no maskable
interrupt arrives in them. An interrupt or exception delivered through
`RSP0` executes `SWAPGS` on entry and exit only when the saved CS has RPL
3. NMI, double fault and machine check can arrive inside the windows, when
CS says CPL 0 but GS may hold the user's base, so their handlers never
rely on GS: each finds its CPU's per-CPU block through the pointer at the
top of its IST stack (S2), and leaves GS as it found it.

- **Why:** the windows are as short as the instructions allow, and the
  only events that can arrive in them have stacks of their own and a way
  to find per-CPU data without GS.
- **Cost:** each call pays two `SWAPGS` and a stack switch in software,
  and the speculative-execution measures of S9 where the processor needs
  them.
- **Not taken:** deciding in the NMI and machine-check handlers whether to
  execute `SWAPGS` from the saved CS, as the other handlers do. Inside the
  windows that rule swaps the wrong way; reading the current GS base
  instead works but costs an MSR read on every NMI.

## S4. Register convention

**Decided:** user code follows the Microsoft x64 calling convention, as
every nova-os image does
([README.md](README.md#executable-format-and-calling-convention)).
**Decided:** the system-call registers.

| Register | At `SYSCALL` | After return |
|---|---|---|
| RAX | Call number | Status (S7), zero-extended from 32 bits |
| R10 | Argument 1 | 0 |
| RDX | Argument 2 | Result 1, or 0 |
| R8 | Argument 3 | Result 2, or 0 |
| R9 | Argument 4 | Result 3, or 0 |
| `[RSP+0x28]`, `+0x30`, `+0x38`, `+0x40` | Arguments 5 to 8 | Unchanged |
| RCX | (written by `SYSCALL`) | The return address |
| R11 | (written by `SYSCALL`) | The caller's RFLAGS |
| RBX, RBP, RDI, RSI, RSP, R12 to R15 | | Preserved |
| All x87, SSE and AVX state, including MXCSR | | Preserved |

The kernel reads only the low 32 bits of an argument declared 32-bit. In
the first set the kernel enables x87, SSE and AVX state in XCR0 (SDM 2.6)
and nothing beyond, so AVX-512, AMX and APX registers do not exist for
user code.

Each call is a function in the native layer, the user-mode library that
holds the system-call stubs. A stub is a leaf function under the x64
calling convention: it allocates no stack, so at the `SYSCALL` its RSP is
what it was at entry, with the return address at `[RSP]`, the caller's
32-byte home area from `[RSP+0x08]` to `[RSP+0x27]`, and the fifth and
later C arguments from `[RSP+0x28]` (x64 calling convention, "Parameter
passing"; x64 stack usage). The kernel therefore finds arguments 5 to 8
where the caller's compiler put them. A stub returns a result through an
output pointer by keeping the pointer in its own home area, which the
callee owns ("these 4 stack locations are effectively owned by the called
function", x64 stack usage), and storing the result register there after
`SYSCALL`:

```asm
; UINT32 handle_duplicate(UINT32 handle, UINT32 rights, UINT32 *out);
handle_duplicate:
    mov  [rsp+0x18], r8     ; keep out in the home slot for R8
    mov  r10, rcx           ; argument 1
    mov  eax, 2             ; call number (S8)
    syscall
    mov  rcx, [rsp+0x18]
    mov  [rcx], edx         ; result 1: the new handle
    ret                     ; status in EAX
```

The stub stores the result even when the status is an error, in which case
it is 0.

Where the system-call convention departs from the x64 calling convention:

1. Argument 1 is in R10, not RCX, because `SYSCALL` overwrites RCX with
   the return address (SDM 6.8.8).
2. RAX carries the call number in.
3. RDX, R8 and R9 carry results out, besides the status in RAX.
4. Volatile registers come back defined: results, zero, or RCX and R11 as
   `SYSRET` leaves them. The x64 convention lets a callee leave any value in
   RAX, RCX, RDX and R8 to R11 ("Caller/callee saved registers").
5. All x87, SSE and AVX state is preserved. The x64 convention treats as
   volatile the x87 register stack, XMM0 to XMM5, the upper halves of the
   YMM and ZMM registers, registers 16 to 31, and MXCSR's six status flags
   ("Caller/callee saved registers", "MXCSR").

- **Why zero the volatile registers:** a register the kernel neither
  defines nor restores would carry whatever kernel code left there to user
  mode. Liedtke recommends defining registers not used for parameters as
  scratch so that the kernel need not save and restore them (Liedtke
  1993, 5.4.2); zeroing keeps that saving and leaks nothing.
- **Why results in registers:** a scalar result never has to be written to
  user memory by the kernel, so a call whose effect has happened (a handle
  created) cannot then fail on a bad output pointer and lose the result;
  if the stub's store faults, the fault is the caller's, in user mode.
- **Why preserve vector state:** the kernel uses no floating point or
  vector instructions ([boot-protocol.md](boot-protocol.md#flags-and-floating-point)),
  so preserving it costs nothing on the call path. **Cost:** the kernel
  can never use them on a call path without saving the user's state
  first.
- **Not taken:** arguments 5 and up in nonvolatile registers. The stub
  would have to save and restore them around every call, since the x64
  convention requires a callee to preserve RBX, RBP, RDI, RSI and R12 to
  R15 ("Caller/callee saved registers").

## S5. The SYSRET hazard

**Decided:** the kernel never executes `SYSRET` with a non-canonical RCX
(SDM 6.8.8). **Decided:** how.

`SYSRET` loads RIP from RCX, and raises #GP(0) if RCX is not canonical
(SDM 6.8.8). The #GP is a fault: the processor restores its state to what
it was before the instruction (SDM 4.5.2, 7.5), which is CPL 0, the
user's RSP the kernel has just loaded, and the user's GS base after
`SWAPGS`. A handler at the same privilege level runs on the current stack
(SDM 7.12.1), so the kernel's #GP handler would run on a stack the user
controls. SDM 6.8.8 requires the operating system to prevent exactly this
and lists three remedies.

RCX can be non-canonical in two ways: `SYSCALL` saves the address of the
next instruction, which is the first non-canonical address if the
`SYSCALL` instruction ends at the top of the lower canonical half (with
4-level paging an address is canonical when bits 63:47 are identical, with
5-level paging bits 63:56, SDM 4.5); and any path that changes a thread's
return address can set it to anything.

nova-os closes both:

1. **The top page is never user memory.** User mappings lie in
   [`USER_BASE`, `USER_TOP`), where `USER_TOP` is the lower half's top
   minus one page: 0x0000_7FFF_FFFF_F000 with 4-level paging and
   0x00FF_FFFF_FFFF_F000 with 5-level paging. No instruction can end above
   `USER_TOP`, so the address `SYSCALL` saves is canonical. This is SDM
   6.8.8's second remedy, "using paging to ensure that the `SYSCALL`
   instruction will never save a non-canonical value into RCX".
2. **Kernel-chosen addresses are checked when they are chosen.**
   `thread_start` and `process_start` reject an entry point or stack at or
   above `USER_TOP` with `ERR_INVALID_ARGS`.
3. **The exit path checks anyway.** Before loading RCX, the exit path
   compares the return address with `USER_TOP`. An address above it is a
   kernel bug; the thread's process is terminated with a fault exit code
   instead of returning, by either `SYSRET` or `IRETQ`. `USER_TOP` itself
   is a legitimate return address, after a `SYSCALL` in the last bytes
   below it, and is canonical. This is SDM 6.8.8's first remedy,
   "confirming that the value of RCX is canonical before executing
   `SYSRET`".
4. **Flags are sanitized.** `SYSRET` loads RFLAGS from R11 (SDM 6.8.8).
   The exit path returns the flags `SYSCALL` saved, keeping only CF, PF,
   AF, ZF, SF, TF, DF, OF, AC and ID, forcing IF and bit 1 to 1, and
   clearing IOPL, NT and RF. With IOPL 3 in R11 a user thread would gain
   every I/O port, because a program whose CPL is at most IOPL may access
   the I/O space, and user code cannot otherwise change IOPL (SDM 2.3).
5. **NMI, double fault and machine check use IST stacks** (S2), so they
   never run on a user stack in either window, as SDM 6.8.8 recommends for
   NMI. Maskable interrupts are excluded by IF (S3). #DB has no IST stack:
   a data breakpoint on a `MOV SS` just before `SYSCALL` would be delivered
   after it (SDM 7.8.3), but user code cannot program the debug registers
   in the first set. A call that lets it must give #DB an IST stack.

- **Why:** each measure closes one way RCX, R11 or an exception could
  carry user-controlled state into a CPL 0 path; together they need no IST
  for #GP and cost one compare on the common path.
- **Cost:** the top page of the user address space is unusable, and every
  exit pays a compare.
- **Not taken:** returning through `IRETQ` always. `IRETQ` loads the
  stack pointer as part of the instruction, so a fault in it is reported
  on the kernel stack; but it is still reported at CPL 0 with the user's
  GS base already swapped in, so the return address must be checked
  either way, and the check is what closes the hazard. The IST remedy is
  discussed in S2.

The SDM is the source for this behaviour. The AMD manual's instruction
reference (Volume 3), which describes `SYSRET`'s faults on AMD processors,
is not pinned; the measures above do not depend on where a processor
reports the fault.

## S6. User memory

**Decided.**

1. Every pointer and length a call takes, and the user-stack range
   arguments 5 to 8 are read from (S3), must describe a range inside
   [`USER_BASE`, `USER_TOP`) without wrapping, or the call fails with
   `ERR_BAD_ADDRESS`. `USER_BASE` is 0x1_0000; the first 64 KiB are never
   mapped, so null-pointer accesses fault.
2. The kernel copies every input structure into kernel memory once, then
   validates the copy, so another thread cannot change it between check
   and use. NT's native services likewise treat every user-mode argument
   as untrusted and probe caller-supplied buffers before using them
   ([Nt and Zw][ms-ntzw]).
3. A page fault in the kernel's routines for copying to and from user
   memory makes the call fail with `ERR_BAD_ADDRESS`; it is never a kernel
   fault.
4. A call that writes to user memory (`channel_read`, `channel_call`,
   `object_wait_many`, `port_wait`, `section_pin`) either completes or
   fails with `ERR_BAD_ADDRESS` leaving kernel state unchanged: a message
   stays queued, a packet stays on its port. The contents of a buffer it
   had begun to fill are then unspecified.
5. Where the processor supports them (CPUID.07H.00H:EBX.SMEP[7] and
   SMAP[20], SDM 5.1.4) the kernel sets CR4.SMEP, so it never executes
   user pages, and CR4.SMAP, so it reads and writes user pages only where
   it sets RFLAGS.AC on purpose (SDM 5.1.3, 2.3). AC is set only inside
   the copy routines; `IA32_FMASK` clears it on entry (S2).

- **Why:** a user pointer is an argument like any other and is checked
  before use; SMAP turns a missed check into a fault instead of a silent
  access.
- **Cost:** item 4 obliges the kernel to reserve before it commits (a
  handle-table slot before a handle moves, a packet before it leaves the
  port).
- **Not taken:** probing every buffer before acting and then writing
  without fault handling. Another thread can unmap the buffer between the
  probe and the write, so the copy routines must handle faults anyway.

## S7. Error reporting

**Decided.**

Every call returns a 32-bit status in EAX, zero-extended into RAX. 0 is
success; any other value is one of the codes below. A call that fails
changes nothing, except where its description says otherwise, and its
result registers are 0 except where its description says otherwise
(`ERR_BUFFER_TOO_SMALL` reports the sizes needed, and `object_wait_one`
cancelled by a closing handle the signals it observed,
[objects.md O9](objects.md#o9-waiting)).

| Code | Name | Meaning |
|---|---|---|
| 0 | `OK` | Success |
| 1 | `ERR_NOT_SUPPORTED` | No such call, the object's type does not support the operation, or a handle that may not travel in this message |
| 2 | `ERR_INVALID_ARGS` | An argument outside its domain: unknown flag bits, a size of zero or past a limit, a misaligned address, an entry point at or above `USER_TOP` |
| 3 | `ERR_BAD_ADDRESS` | A buffer outside the user range or not mapped with the access the call needs |
| 4 | `ERR_BAD_HANDLE` | The value names no handle in the caller's table |
| 5 | `ERR_WRONG_TYPE` | The handle names an object of another type |
| 6 | `ERR_ACCESS_DENIED` | The handle lacks a right the call needs, or the rights asked for are not a subset of the handle's |
| 7 | `ERR_NO_MEMORY` | Kernel memory, or the job's quota of handles, objects, message bytes or pages, is exhausted |
| 8 | `ERR_OUT_OF_RANGE` | A count past a fixed limit (`CHANNEL_MAX_BYTES`, `CHANNEL_MAX_HANDLES`, `WAIT_MANY_MAX`, `DEBUG_WRITE_MAX_BYTES`), or an offset past an object's size or a resource's range |
| 9 | `ERR_SHOULD_WAIT` | The operation would block: a queue is empty or full |
| 10 | `ERR_TIMED_OUT` | The deadline passed |
| 11 | `ERR_CANCELED` | A handle being waited on was closed, or a request was cancelled |
| 12 | `ERR_PEER_CLOSED` | The other end of a channel is closed |
| 13 | `ERR_BUFFER_TOO_SMALL` | A receive buffer cannot hold the message |
| 14 | `ERR_BAD_STATE` | The object's state forbids the call: a thread already started, an interrupt already bound, a process already terminated |
| 15 | `ERR_NO_RESOURCES` | A resource other than memory is exhausted: the job's quota of interrupt vectors, or the vectors the kernel has for message-signalled interrupts |

The same codes report the outcome of an I/O request in its completion
message ([ipc.md I9](ipc.md#i9-asynchronous-io-requests)). A Win32-style
subsystem translates them to its own codes in user mode.

- **Why:** a caller tests one value against zero. The codes name the
  checks the kernel makes, in the order objects.md O3 makes them. Zircon's
  errors are negative today, and its documentation states they "will
  transition to using 0 for success and positive values for errors"
  ([Errors][zx-errors]), the form used here.
- **Cost:** a call cannot report partial success with a warning; it
  succeeds, or fails having changed nothing.
- **Not taken:** NTSTATUS's layout: a 2-bit severity (success,
  informational, warning, error), a customer bit, a reserved bit, a 12-bit
  facility and a 16-bit code ([MS-ERREF 2.3][ms-erref]), tested with
  `NT_SUCCESS` and related macros ([Using NTSTATUS Values][ms-ntstatus]).
  Its costs are a
  range test where a zero test would do ("a common mistake ... is to
  compare the routine's return value with `STATUS_SUCCESS`", the same
  page), and warning and informational successes that let a call succeed
  partway. seL4 reports errors in the reply's message tag with details in
  message registers (seL4 10.1), which suits its message-based calls and
  not a register return.

## S8. The first calls

**Decided.** Numbers are the values of RAX. Number 0 is reserved and
always fails with `ERR_NOT_SUPPORTED`, so a stub that never loads RAX runs
no call. "Results" are RDX, R8 and R9 in order; "writes" in the last
column marks an array the kernel fills in user memory.

| # | Call | Arguments | Results | Needs | In the core because |
|---|---|---|---|---|---|
| 1 | `handle_close` | handle | | | The handle table is kernel state |
| 2 | `handle_duplicate` | handle, rights | handle | `DUPLICATE` | Same |
| 3 | `handle_restrict` | handle, rights | | | Same |
| 4 | `handle_info` | handle | type, rights, object ID | | Same |
| 5 | `object_signal` | handle, clear mask, set mask | | `SIGNAL` | Waking waiters needs the scheduler |
| 6 | `object_wait_one` | handle, signals, deadline | observed signals | `WAIT` | Blocking needs the scheduler |
| 7 | `object_wait_many` | items, count, deadline | | `WAIT` on each | Same; writes each item's observed signals |
| 8 | `object_wait_async` | handle, port, key, signals | | `WAIT`; `QUEUE` on the port | Packets come from kernel state changes |
| 9 | `event_create` | | handle | | A waitable object |
| 10 | `channel_create` | capacity | handle, handle | | Messages carry handles between tables |
| 11 | `channel_write` | channel, bytes, byte count, handles, handle count | | `WRITE`; `TRANSFER` on each handle | Same |
| 12 | `channel_read` | channel, bytes, byte capacity, handles, handle capacity | byte count, handle count | `READ` | Same; writes the message |
| 13 | `channel_call` | channel, arguments, deadline | byte count, handle count | `READ`, `WRITE` | Same; writes the reply |
| 14 | `port_create` | capacity | handle | | Interrupts and watches deliver into it |
| 15 | `port_queue` | port, packet | | `QUEUE` | Same |
| 16 | `port_wait` | port, deadline, packet | | `READ` | Same; writes the packet |
| 17 | `clock_monotonic` | | nanoseconds | | The kernel owns the time base (`BOOT_INFO.tsc_per_ms`) |
| 18 | `job_create` | parent job, limits | handle | `CREATE_JOB` | Kernel-memory quota |
| 19 | `process_create` | job, options | handle | `CREATE_PROCESS`; `WRITE_EXECUTE` for `PROCESS_OPTION_WRITE_EXECUTE` | Address spaces and handle tables |
| 20 | `process_start` | process, thread, entry, stack, bootstrap handle, argument | | `CREATE_THREAD`; `START` on the thread; `TRANSFER` on the bootstrap handle | Starts a thread and moves a handle between tables |
| 21 | `process_exit` | exit code | (does not return) | | Termination |
| 22 | `process_info` | process | state, exit code | `INSPECT` | Process state is kernel state |
| 23 | `thread_create` | process | handle | `CREATE_THREAD` | Scheduling |
| 24 | `thread_start` | thread, entry, stack, argument 1, argument 2 | | `START` | Same |
| 25 | `thread_exit` | | (does not return) | | Same |
| 26 | `task_kill` | process, thread or job | | `TERMINATE` | Same |
| 27 | `section_create` | size, options | handle | | Physical memory |
| 28 | `process_map` | process, section, offset, address, size, protection | address | `MAP`; section rights for the protection | Page tables |
| 29 | `process_unmap` | process, address, size | | `MAP` | Same |
| 30 | `process_protect` | process, address, size, protection | | `MAP`; the protection within the rights the mapping was made with ([objects.md O5](objects.md#o5-access-rights)) | Same |
| 31 | `resource_create` | parent resource, base, size | handle | `USE` | Device authority ([ipc.md I6](ipc.md#i6-device-memory-and-io-ports)) |
| 32 | `section_create_physical` | resource, base, size, cache policy | handle | `USE` on an MMIO resource | Page tables, memory types |
| 33 | `interrupt_create` | resource, source, options | handle, MSI address, MSI data | `USE` on an IRQ resource | Interrupt routing ([ipc.md I7](ipc.md#i7-interrupts)) |
| 34 | `interrupt_bind` | interrupt, port, key | | `BIND`; `QUEUE` on the port | Same |
| 35 | `interrupt_ack` | interrupt | | `ACK` | Unmasking needs privilege |
| 36 | `ioport_set` | resource, base, count, allow or deny | | `USE` on an I/O-port resource | The TSS I/O permission bitmap |
| 37 | `initiator_create` | resource, requester ID | handle | `USE` on a DMA resource | DMA authority ([ipc.md I8](ipc.md#i8-dma)) |
| 38 | `section_pin` | initiator, section, offset, size, options, addresses | handle | `PIN` on both; `READ` or `WRITE` on the section | Page lifetime, IOMMU; writes device addresses |
| 39 | `pin_release` | pin | | | Same |
| 40 | `initiator_release_quarantine` | initiator | | `RELEASE` | Same |
| 41 | `debug_write` | resource, bytes, byte count | | `USE` on the debug-log resource | The kernel's serial console |
| 42 | `random_read` | bytes, byte count | | | The kernel's own generator, which it needs before any server runs; results are recorded for replay ([replay.md](replay.md)), as with Zircon's `zx_cprng_draw` ([cprng_draw][zx-cprng]) |
| 43 | `task_exception_channel` | process or job | handle | `TERMINATE` | Faults are raised by the processor and reach the kernel first ([objects.md O10](objects.md#o10-object-lifetime)) |
| 44 | `thread_set_tls_base` | base | | | Without `WRFSBASE`, a 64-bit segment base is loaded with `WRMSR`, at CPL 0 only (S10) |

`job_create`'s limits is the address of a `JOB_LIMITS` record in user
memory: `interrupt_vectors`, the message-signalled vectors that the
processes in the new job and its own child jobs may hold at once. The
limits are reserved from what the parent job has left, and the call
fails with `ERR_NO_RESOURCES` if it has too few; they return to the
parent when the job is destroyed ([objects.md O6](objects.md#o6-no-ambient-authority)).
The root job holds every vector the kernel has for message-signalled
interrupts. The other quotas O6 lists join the record as the kernel
enforces them.

`process_create`'s options are `PROCESS_OPTION_32_BIT` (1), whose threads
run 32-bit code (S11), and `PROCESS_OPTION_WRITE_EXECUTE` (2), whose
mappings may be writable and executable at once
([objects.md O5](objects.md#o5-access-rights)); another bit fails with
`ERR_INVALID_ARGS`. `section_create` rounds `size` up to whole pages and
fills them with zeros; `SECTION_MAX_SIZE` (2 MiB) is its limit, and no
option is defined yet. `process_map` takes a fixed address, page-aligned
like `offset` and `size`, and a protection of `MAP_READ` (1) with
`MAP_WRITE` (2), `MAP_EXECUTE` (4), both or neither; a range that is
partly mapped already fails with `ERR_BAD_STATE`, and its result is the
address. A mapping holds its section until the process terminates.

`section_create_physical`'s cache policy is `CACHE_UNCACHED` (0) or
`CACHE_WRITE_COMBINING` (1), and its limit is `SECTION_MAX_SIZE` too, so
mapping a device section takes no longer than mapping a memory one; a
larger range takes several sections. `section_pin`'s options are
`PIN_DEVICE_READ` (1), `PIN_DEVICE_WRITE` (2) or both, and it writes one
`UINT64` device address per page. `interrupt_create` takes `options` 0
for a message-signalled source, numbered as
[ipc.md I6](ipc.md#i6-device-memory-and-io-ports) gives, and fails with
`ERR_NOT_SUPPORTED` for a global system interrupt, which this kernel does
not route yet. The vector it allocates is charged to the calling
process's job until the interrupt's last handle closes; past the job's
quota, or with no vector free, the call fails with `ERR_NO_RESOURCES`. `pin_release` ends the pin and removes its handle
together, so no other thread of the process can quarantine it between
the two.

`thread_start` starts a further thread of a process `process_start` has
started; it fails with `ERR_BAD_STATE` if the thread has been started, or
if its process was never started or has begun terminating
([objects.md O10](objects.md#o10-object-lifetime)).

A new thread, from `process_start` or `thread_start`, begins at `entry`
with RCX and RDX holding its two arguments, every other general-purpose
register zero, RFLAGS 0x202 (IF and bit 1), and RSP equal to `stack`.
`stack` must be 8 more than a multiple of 16 with at least 40 bytes mapped
above it, the state the x64 calling convention gives a called function:
RSP 16-byte aligned before the call pushed the return address, and room
for the return address and the 32-byte home area (x64 stack usage). The
x87 control word is 0x027F and MXCSR 0x1F80, the standard values the
convention gives the start of a program (all exceptions masked, round to
nearest, double precision for x87; "FPCSR", "MXCSR"), and every other x87,
SSE and AVX register is zero. For `process_start` the first argument is
the bootstrap handle's value in the new process, as Zircon places it in
the first argument register ([zx_process_start][zx-pstart]). The entry
function must not return.

- **Why this set:** each call passes objects.md O1's test, shown in the
  last column; a call is added only when a first-stage component needs it
  and no user-mode server can provide it.
- **Cost:** 44 calls to implement and test before the first user-mode
  driver runs.
- **Not taken:** seL4's form, a handful of system calls with every object
  operation sent as a message to the object's capability (seL4 2.2). Its
  cost here is encoding and decoding a message for every operation, and
  per-type method dispatch in place of one call table.

The native layer's C functions are the system's programming interface;
the numbers and the register convention are private to it. Zircon goes
further and makes its vDSO "the only means to enter the kernel for system
calls", enforcing that every entry comes from the vDSO's code
([vDSO][zx-vdso]); nova-os does not enforce it (**Decided**).

## S9. Speculative execution

**Decided.** The entry and exit of S3, the interrupt and exception entries
from user mode, and the switch from one process to another carry the
speculative-execution mitigations of [speculation.md](speculation.md). Its
M3 lists them by path and in their order among S3's steps; its later
sections give, for each, the processors it applies to, how the kernel
detects them, and the vendor guidance it follows.

- **Why:** the same measures apply to every entry from user mode, not only
  to `SYSCALL`, and to the process switch, so they are specified once,
  apart from this path.
- **Cost:** on some processors they dominate the cost of a call: an IBPB
  on every entry on AMD processors affected by branch type confusion or
  speculative return stack overflow ([speculation.md M9](speculation.md#m9-branch-type-confusion),
  [M10](speculation.md#m10-speculative-return-stack-overflow)).

## S10. Thread-local storage

**Decided.**

A user thread's thread-local storage base is its FS base. A thread
starts with base 0 and sets its own with `thread_set_tls_base(base)`
(S8); `base` must be 0 or an address in [`USER_BASE`, `USER_TOP`), or the
call fails with `ERR_INVALID_ARGS`. The kernel never reads memory at the
base. The base is part of the thread's user state, like its vector
registers (S4): the kernel saves `IA32_FS_BASE` (C000_0100h, APM 4.5.3)
when it switches away from the thread and loads it when it switches
back, so the thread keeps whatever base it last had, however it was set.
In compatibility mode an FS override adds the base's low 32 bits (SDM
3.4.4), which is how a 32-bit thread reaches its TEB (S11).
CR4.FSGSBASE (bit 16) stays clear, so `RDFSBASE`, `WRFSBASE`, `RDGSBASE`
and `WRGSBASE` are not enabled (SDM 2.5); loading a non-null selector
into FS still sets the base, from the descriptor's 32-bit base (SDM
3.4.4), which is 0 in every descriptor user code may load.

- **Why a call:** `WRFSBASE` needs a processor that reports it and the
  kernel's CR4.FSGSBASE (SDM 2.5). QEMU's default processor model
  reports no FSGSBASE (CPUID.07H.00H:EBX is 0 there), so a call is needed
  on such a processor in any case, and one mechanism serves every x86-64
  processor. A thread sets its base once, when its runtime sets up its
  thread-local storage.
- **Why FS:** the kernel uses GS for its per-CPU block (S3) and never
  FS, so the user's base never passes through `IA32_KERNEL_GS_BASE`, and
  `SWAPGS` keeps one job.
- **Cost:** a system call per change of base, where `WRFSBASE` is one
  instruction; and on each switch between user threads, a read and a
  write of `IA32_FS_BASE`. TinyCC at the pinned commit does not implement
  `_Thread_local` (`tccgen.c`), so the native layer, not the compiler,
  reaches the base.
- **Not taken:** CR4.FSGSBASE set where the processor reports it, with
  the call kept for processors that do not. Its cost is two mechanisms,
  and user code able to set GS's base as well, which the kernel would
  then also save and restore on every switch.

## S11. The 32-bit lane

**Decided.**

A process created with `PROCESS_OPTION_32_BIT` runs 32-bit code: its
threads start in compatibility mode with CS 0x1B, whose descriptor has L
clear and D set, so their default operand and address size is 32 bits
(SDM 6.2.1, 12.8.5.3), and SS, DS and ES 0x23, a flat 4 GiB data segment
with B set, so the stack is 32-bit (SDM 3.4.5). Such a thread enters the
kernel with `INT 0x80` (`COMPAT_GATE_VECTOR`) and leaves through the
`IRETQ` exit of every interrupt from user mode, which returns to
compatibility mode because the frame's CS is 0x1B (SDM 7.14.3).

The gate is a 64-bit interrupt gate of DPL 3, the only gate user code may
raise with `INT n`; every other vector's gate has DPL 0, so `INT n`
there raises #GP (SDM 7.12.1.2). As an interrupt gate it clears IF, which
stays clear until the entry has swapped GS (S3); the call then runs with
interrupts enabled, as a `SYSCALL` does. The kernel never routes an
interrupt source to vector 0x80, so an event there is always the `INT`.

| Register | At `INT 0x80` | After return |
|---|---|---|
| EAX | Call number (S8) | Status (S7) |
| EDX | Address of the arguments: one 8-byte little-endian value each, as many as S8 gives the call | Result 1 |
| ECX | | Result 2 |
| EBX, ESI, EDI, EBP, ESP | | Preserved |
| All x87 and SSE state | | Preserved |

The kernel reads only the low 32 bits of EAX and EDX, since the upper 32
bits of a general register are undefined once compatibility-mode code
has run (SDM Vol. 1 3.4.1.1), copies the arguments with the user-memory
routines (S6), and returns each result zero-extended. A call is served
on the lane only if every result fits in 32 bits and a 32-bit process
has a use for it: `handle_close`, `object_wait_one`, `object_wait_many`,
`channel_create`, `channel_write`, `channel_read`, `process_exit`,
`process_info`, `thread_exit`, `debug_write` and `thread_set_tls_base`.
Any other number fails with `ERR_NOT_SUPPORTED`. Every pointer and
length is checked as S6 checks a 64-bit caller's.

Compatibility mode uses DS, ES, FS and GS, and faults on any access
through one that holds a null selector (SDM 6.4.1), while 64-bit mode
ignores them but for FS's and GS's bases (SDM 3.4.4, 6.4.1.1). User code
of either mode can load them, so each user thread keeps its own four
selectors, which the kernel saves and loads with its FS base on every
switch, and one thread's choice never reaches another's code.

A new thread of a 32-bit process begins at `entry` with ESP equal to
`stack`, ECX and EDX its two arguments, the other general registers
zero, EFLAGS 0x202, the user data selector 0x23 in DS, ES, FS and GS,
and FS base 0. `entry` and `stack` must lie below
4 GiB, the most a compatibility-mode address reaches, and `stack` must be
a multiple of 4, the size every x86 argument is widened to ("Argument
Passing and Naming Conventions", Microsoft Learn), or `process_start`
fails with `ERR_INVALID_ARGS`.

A thread of a 64-bit process that executes `INT 0x80`, in whatever mode
its code has reached, ends its process with exit code 13, as the #GP of a
DPL 0 gate would. A 32-bit process's thread that executes `SYSCALL` ends
its process with exit code 6 (S2), and one that far-jumps to 64-bit code
reaches the `SYSCALL` entry like any 64-bit thread.

- **Why `INT`:** it is the one entry that works from compatibility mode
  on both vendors. Intel processors do not support `SYSCALL` there (SDM
  6.8.8), and AMD processors raise #UD for `SYSENTER` in long mode (APM
  6.1.2). The processor switches to the kernel stack and saves the stack
  pointer itself (SDM 7.14.4), so S5's windows do not arise.
- **Why results in two registers:** EAX, ECX and EDX are the registers a
  called x86 function may change ("Argument Passing and Naming
  Conventions"); the others are preserved, so a stub saves nothing.
- **Why a process option:** the lane is the 32-bit process's interface,
  and a 64-bit process gains nothing from it that `SYSCALL` lacks; the
  kernel decides from the process, which user code cannot change, and
  not from the mode the code runs in, which any user code can change by
  a far transfer to the 32-bit descriptor.
- **Cost:** a gate and segment descriptor load per call (S1), and the
  arguments read from memory.
- **Not taken:** a far transfer to 64-bit user code that then executes
  `SYSCALL`, as a 64-bit layer under 32-bit code would do. It needs a
  64-bit user-mode library in every 32-bit process. A call gate into the
  kernel, which IA-32e mode allows (SDM 2.1.2.1): it changes neither GS nor
  IF, so it would need the windows S3 closes for `SYSCALL`.

The gate enters through the same path as every interrupt from user mode
(S9), with the same measures: RFLAGS.AC cleared, `SWAPGS` decided by the
saved CS, and the return address checked against `USER_TOP` (S5). None
of the per-entry and per-exit measures of
[speculation.md M3](speculation.md#m3-the-measures-by-path) is in the
kernel yet on any path, the `SYSCALL` entry included: the LFENCE after
the conditional `SWAPGS` (M5), IBRS writes (M6), IBPB (M9, M10), the
32-CALL overwrite (M7), the BHB-clearing sequence (M8), page-table
isolation (M11), VERW (M13) and the masked call number (M4). The lane
takes each when the interrupt entry does.

## References

[Using Nt and Zw Versions of the Native System Services Routines][ms-ntzw]
(Microsoft Learn, 2025-04-30);
[Using NTSTATUS Values][ms-ntstatus] (Microsoft Learn, 2025-11-05);
[MS-ERREF 2.3 NTSTATUS][ms-erref] (Microsoft Learn, 2024-10-30);
[Zircon Errors][zx-errors] (Fuchsia, 2025-02-28);
[zx_process_start][zx-pstart] (Fuchsia, 2025-03-04);
[Zircon vDSO][zx-vdso] (Fuchsia, 2025-03-22).

[ms-ntzw]: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-nt-and-zw-versions-of-the-native-system-services-routines
[ms-ntstatus]: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-ntstatus-values
[ms-erref]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-erref/87fba13e-bf06-450e-83b1-9241dc81e781
[zx-errors]: https://fuchsia.dev/fuchsia-src/concepts/kernel/errors
[zx-pstart]: https://fuchsia.dev/fuchsia-src/reference/syscalls/process_start
[zx-cprng]: https://fuchsia.dev/fuchsia-src/reference/syscalls/cprng_draw
[zx-vdso]: https://fuchsia.dev/fuchsia-src/concepts/kernel/vdso
