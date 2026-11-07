# Record and replay

DESIGN.md principle 8 has every effect a program has outside its own
address space go through a handle or a message, so that a session can be
recorded and replayed. A recording is the sequence of nondeterministic
inputs a set of processes receives; replaying the same inputs in the same
order reproduces their execution. This document lists every way a
nondeterministic input reaches a process and how each one is recorded or
excluded. The system calls it names are in [syscalls.md](syscalls.md), and
channels and ports in [ipc.md](ipc.md).

## Inputs through the kernel

**Decided.** Each of these crosses the system-call boundary, so the kernel
records it where it delivers it:

- the results of every system call, including `clock_monotonic`, the only
  way a process reads the time;
- every message and handle a process reads from a channel;
- every packet a process reads from a port, interrupts included.

## Instructions that bypass the kernel

Some instructions return nondeterministic values at CPL 3 without entering
the kernel.

- **The timestamp counter. Decided.** The kernel sets CR4.TSD (bit 2)
  during initialization, which restricts RDTSC, and RDTSCP where present,
  to privilege level 0 (SDM Vol. 3, the CR4 description and the
  time-stamp counter section). The SDM's guidance is the same for any
  secure system: set TSD at initialization and offer the time through an
  interface, here `clock_monotonic`. It applies to every process, recorded
  or not.
- **Performance counters. Decided.** CR4.PCE (bit 4) stays clear, so
  RDPMC is restricted to privilege level 0 (SDM Vol. 3, the CR4
  description).
- **Hardware random numbers. Decided.** The pinned SDM volume gives no
  way to stop RDRAND or RDSEED at CPL 3 outside VMX operation, where a
  VM-execution control can intercept them. A recorded program that
  executes them is not replayable. Programs get random bytes from the
  kernel's generator through `random_read`, whose results are recorded
  like any other system call's, and a recording reports whether the
  processor supports RDRAND, so whether recorded code could have bypassed
  the call. Hiding RDRAND from CPUID (CPUID faulting) is documented
  outside the pinned volume and is unverified here.
- **CPUID. Decided.** Its results differ between machines, so a recording
  carries the CPUID results the recorded process could observe, and replay
  on a machine with different results is reported as unfaithful rather
  than attempted.

## Thread interleaving

**Decided.** The order in which threads of one process touch shared
memory is an input. While a process is recorded, the kernel runs at most
one of its threads at a time and records each point at which it switches
between them, so replay can switch at the same points. Locating a
preemption point within a thread's execution on replay needs a
deterministic count of the work done, such as a retired-instruction or
retired-branch performance counter; which counter is exact enough on the
target processors is unverified.

## Shared memory

**Decided.** A section mapped writable into a recorded process and into a
process outside the recording delivers the other process's writes as
inputs the kernel never sees. A recorded process therefore cannot map a
writable section shared with a process outside the recording; read-only
sharing, such as store paths, is unaffected.

## Recordings

**Decided.** A recording holds every message and system-call result its
processes received, random bytes and the keys derived from them included,
so it is as sensitive as the session it records. A recording is reached
only through a handle, like any other object, and is never written where
the recorded processes' own rights would not let it be read.

## Devices

**Decided.** A driver's loads and stores to device memory and its port
I/O are not mediated access by access, so they are not recorded. A driver
is recorded at its message boundary: the requests it receives and the
completions and interrupts it delivers. Replay of a driver alone needs a
device model, which this document does not specify.
