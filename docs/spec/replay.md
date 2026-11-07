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

**Decided.** A section that a process outside the recording can write
delivers that process's writes as inputs the kernel never sees. So no
section a recorded process maps may be writable by a process outside the
recording: through a handle that carries `WRITE`, a writable mapping, or
a pin that lets a device write it ([ipc.md I8](ipc.md#i8-dma)). A section
the recorded process writes and every other process only reads, such as
a window's buffer, which the compositor maps read only
([display.md D6](display.md#d6-windows-and-buffers)), is its output, not
an input; read-only sharing, such as store paths, is unaffected.

- **Why:** what makes shared memory an input is a writer outside the
  recording. Replay reproduces the recorded process's own writes, so a
  section only it writes holds nothing a replay cannot rebuild, whoever
  else reads it.
- **Cost:** the kernel must tell, when a recorded process maps a section
  or sends a handle to one, whether any handle, mapping or pin outside
  the recording can write it, the query
  [storage.md F15](storage.md#f15-store-reads-and-replay) asks for; a
  recorded process cannot map a section another process writes for it,
  such as a capture's buffer or a ring a service fills.

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

The compositor and the network service are recorded the same way, at
their message boundaries. The compositor maps sections its clients write
and writes device memory, so it is replayed with its clients or not at
all; a window's buffer is its client's output, and a capture, whose
buffers the compositor writes, cannot be held by a recorded process
([display.md D17](display.md#d17-replay-and-audit)). A program's network
input arrives in messages on its sockets, so the socket protocol is the
network's boundary; the network service, whose frames a device writes by
DMA, is recorded only at its messages, and alone only once its driver
copies each received frame into a message
([network.md N16](network.md#n16-replay)).

- **Why:** each takes input no recording holds, a client's pixels or a
  device's frames, so what a recording can hold of it is the messages it
  exchanges with the processes recorded.
- **Cost:** the composed screen and the network's traffic are not
  replayed from a recording; a recorded program cannot take a capture;
  recording the network service alone waits on a driver that copies
  frames.
