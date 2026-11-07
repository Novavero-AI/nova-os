# Kernel objects and handles

The objects the nova-os kernel core implements, the handles through which
user code reaches them, the rights a handle carries, the namespace, waiting,
and object lifetime. [ipc.md](ipc.md) specifies channels, ports and how
user-mode drivers use these objects; [syscalls.md](syscalls.md) specifies
the system-call entry and the calls named here.

Each decision gives why, what it costs, and the main alternative not taken
with that alternative's cost. "seL4 4.2" is section 4.2 of the seL4 Reference Manual
and "Liedtke 1995" the paper, at the versions pinned in
[README.md](README.md#design-references). Microsoft Learn
and Fuchsia pages are linked under [References](#references).

## O1. Admission to the core

**Decided** (DESIGN.md, principle 2).

An object type or call belongs in the privileged core only if it needs the
processor's privilege (page tables, interrupt delivery, the timer, mode
switches) or the kernel's exclusive control of physical memory, scheduling
or the handle table. Everything else is a user-mode server reached through
a channel.

- **Why:** DESIGN.md keeps in the kernel "only what needs the processor's
  privilege". The test is Liedtke's: a concept is tolerated inside the
  microkernel only if moving it outside "would prevent the implementation
  of the system's required functionality" (Liedtke 1995, section 2).
- **Cost:** services NT keeps in its kernel (the object namespace, file
  objects, the I/O manager's request routing) become user-mode protocols,
  each with an IPC round trip where NT has a call.
- **Not taken:** NT's arrangement, in which the object manager keeps the
  namespace and the I/O manager builds and routes requests in kernel mode
  ([Object Manager][ms-objmgr], [Windows I/O model][ms-iomodel]). Its cost
  is a privileged core that parses paths, holds names and routes I/O, which
  principle 2 rules out.

## O2. Object types

### First set

| Type | What it is | Why it is in the core | Status |
|---|---|---|---|
| Process | An address space and a handle table; the unit of isolation and of termination | Page tables and the handle table need privilege | Decided |
| Thread | A schedulable processor context running in one process | Scheduling needs privilege | Decided |
| Section | A range of pages that can be mapped into processes: zero-filled memory, or a physical range of device memory | Mapping pages needs privilege; sharing them is how bulk data moves ([ipc.md](ipc.md#i5-bulk-data)) | Decided |
| Event | Eight user-settable signal bits that threads wait on | Waiting across processes needs the scheduler | Decided |
| Channel | A pair of endpoints carrying messages of bytes and handles | Moving handles between handle tables needs the kernel; DESIGN.md keeps messages between processes in the core | Decided |
| Port | A queue of packets: user packets, signal packets from watched objects, interrupt packets | One thread must wait on many sources, including interrupts, which only the kernel can deliver | Decided |
| Job | A node in a tree of processes and jobs that carries a kernel-memory quota and the authority to create processes | Kernel memory is allocated by the kernel, so only the kernel can enforce a limit on it | Decided |
| Resource | Authority over a range of physical addresses, interrupt sources or I/O ports, or a named system privilege | Device access must not be ambient ([O6](#o6-no-ambient-authority)) | Decided |
| Interrupt | One interrupt source, delivered as port packets | Interrupt delivery needs privilege | Decided |
| Initiator | A device's authority to do DMA, used to pin sections for it | Pinning and IOMMU programming need privilege | Decided |
| Pin | Pages of a section held for a device's DMA | The pages' lifetime is kernel state | Decided |

[ipc.md](ipc.md) gives the driver-facing types (resource, interrupt,
initiator, pin) their full rules. A process owns exactly one address space;
there is no separate address-space object (**Decided**): mapping calls take
a process handle, as NT's `ZwMapViewOfSection` takes a `ProcessHandle`
([ZwMapViewOfSection][ms-mapview]). Zircon's alternative, address-space
regions as handles of their own ([VMAR][zx-vmar]), lets a process delegate
part of its address space; its cost is another object type and a tree of
regions in the kernel, which no first-stage use needs.

### Not in the core

| NT or Zircon object | Where it lives in nova-os | Status |
|---|---|---|
| Object directory, symbolic link | The user-mode namespace server ([O8](#o8-the-namespace)) | Decided |
| File, device | User-mode servers; a file is a channel to its file system ([ipc.md](ipc.md#i9-asynchronous-io-requests)) | Decided (DESIGN.md: file systems and drivers are user-mode processes) |
| Timer | Every wait takes a deadline ([O9](#o9-waiting)); a server multiplexes its own timeouts | Decided |
| Mutex, semaphore | In-process locks are user-mode code; cross-process hand-off uses a port, which releases one waiter per packet ([ipc.md](ipc.md#i4-ports)) | Decided |
| Access token, security descriptor | None: authority comes only from handles and from what they were used to set up, such as mappings ([O5](#o5-access-rights)) | Decided (DESIGN.md, principle 7) |

- **Why no timer:** a server that needs timeouts waits on its port with the
  earliest deadline it holds, so a timer object adds no capability that
  waits lack (O1's test). **Cost:** each such server keeps its own timeout
  queue. **Not taken:** NT's waitable timers and Zircon's timer object
  ([Timer][zx-timer]); their cost is one more object type and a kernel
  timer queue shared by all processes.
- **Why no mutex or semaphore:** NT's mutexes and semaphores change state
  when a wait succeeds ([WaitForMultipleObjects][ms-wfmo]: "the count of a
  semaphore object is decreased by one"), which makes waits mutate objects
  and wait-for-all an atomic acquisition. nova-os waits only observe
  ([O9](#o9-waiting)). **Cost:** a later Win32-style subsystem emulates
  named cross-process mutexes over ports or channels.

## O3. Handles

**Decided** that handles are per-process references with rights
(DESIGN.md, principle 2). **Decided** encoding and limits.

A handle is a 32-bit value that names one entry in its process's handle
table. The entry holds a reference to an object and the rights the handle
carries. All threads of a process share its table. A handle value means
nothing in another process.

| Bits | Field |
|---|---|
| 31:8 | Index into the handle table |
| 7:0 | Generation, 1 to 255 |

The kernel increments an entry's generation, skipping 0, each time a
handle in that entry closes, so a stale value is usually rejected with
`ERR_BAD_HANDLE` instead of naming a newer object. Generation 0 is never
issued, so 0 is never a valid handle and serves as `HANDLE_INVALID`. A
process holds at most 2^24 handles (the index width), fewer if its job's
quota is smaller ([O6](#o6-no-ambient-authority)).

Every call that takes a handle checks, in order, that the value names an
entry in the caller's table (`ERR_BAD_HANDLE`), that the object has the
type the call needs (`ERR_WRONG_TYPE`), and that the handle carries the
rights the call needs (`ERR_ACCESS_DENIED`). NT checks a user-mode
handle's granted access the same way when a driver turns it into an object
pointer ([ObReferenceObjectByHandle][ms-obref]); Zircon checks handle,
type and rights on every call ([Kernel concepts][zx-concepts]).

- **Why:** a 32-bit value fits in one argument register with room for a
  generation; the generation turns most use-after-close bugs into errors.
- **Cost:** 255 generations wrap, so stale-value detection is probabilistic.
  It is a debugging aid, not a security boundary: a stale value can only
  name an object the same process already holds.
- **Not taken:** Zircon's encoding, in which the kernel "is free to re-use
  the integer values of closed handles" and a valid value has its two low
  bits set ([Handles][zx-handles]); a reused value silently names a new
  object. seL4 addresses a capability by its slot in a tree of capability
  nodes (seL4 3.3), which lets user code place capabilities where it
  chooses but makes every reference a path the kernel walks.

Each object also has a 64-bit object ID, unique since boot and never
reused, which `handle_info` reports so that a holder can tell whether two
handles name the same object (**Decided**, after Zircon's koid
([Kernel concepts][zx-concepts])).

## O4. Handle operations

**Decided.**

| Call | Effect | Needs |
|---|---|---|
| `handle_close` | Removes the handle from the table and drops its reference | nothing |
| `handle_duplicate` | Adds a second handle to the same object with the same or fewer rights | `DUPLICATE` |
| `handle_restrict` | Narrows the rights of a handle in place; the value is unchanged | nothing |
| `handle_info` | Reports the object's type and ID and the handle's rights | nothing |

Handles move between processes only by channel messages
([ipc.md](ipc.md#i2-handle-transfer)) and by `process_start`, which moves
one bootstrap handle into a new process ([O6](#o6-no-ambient-authority)).

- **Why restrict in place:** a holder can drop a right it does not need
  without holding `DUPLICATE`, without a window in which two handles exist,
  and without invalidating copies of the value other code has recorded.
- **Cost:** two ways to narrow rights, `handle_restrict` and
  `handle_duplicate` with a smaller mask, where one call could serve.
- **Not taken:** Zircon's `zx_handle_replace`, which invalidates the old
  value and returns a new one ([zx_handle_replace][zx-replace]). Its cost
  is that every recorded copy of the old value becomes stale.

## O5. Access rights

**Decided:** rights only narrow (follows from O6). **Decided:** the layout
and the per-type rights.

A handle's rights are a 32-bit mask. Bits 0 to 15 are specific to the
object type; bits 16 to 19 are common to all types; bits 20 to 31 are
reserved and zero.

| Bit | Common right | Allows |
|---|---|---|
| 16 | `DUPLICATE` | `handle_duplicate` |
| 17 | `TRANSFER` | Moving the handle in a channel message or by `process_start` |
| 18 | `WAIT` | `object_wait_one`, `object_wait_many`, `object_wait_async` |
| 19 | `SIGNAL` | `object_signal` on the object's user signals |

| Type | Bit 0 | Bit 1 | Bit 2 | Bit 3 |
|---|---|---|---|---|
| Process | `MAP`: map, unmap and protect in its address space | `CREATE_THREAD` | `TERMINATE` | `INSPECT`: `process_info` |
| Thread | `START` | `TERMINATE` | | |
| Section | `READ` | `WRITE` | `EXECUTE` | `PIN` |
| Channel | `READ` | `WRITE` | | |
| Port | `QUEUE`: `port_queue`, and naming the port in `object_wait_async` or `interrupt_bind` | `READ`: `port_wait` | | |
| Job | `CREATE_PROCESS` | `CREATE_JOB` | `TERMINATE` | `WRITE_EXECUTE`: its processes may map pages both writable and executable |
| Resource | `USE`: derive sub-resources and create objects over the range | | | |
| Interrupt | `BIND` | `ACK` | | |
| Initiator | `PIN` | `RELEASE`: release quarantined pins | | |

Events and pins have no type-specific rights. Mapping a section with a
protection needs the matching section rights: `READ` for read, `WRITE` for
write, `EXECUTE` for execute. A mapping records the section rights of the
handle it was made with, and `process_protect` can change its protection
only within them. No mapping is both writable and executable unless its
process was created with `PROCESS_OPTION_WRITE_EXECUTE`
([syscalls.md S8](syscalls.md#s8-the-first-calls)), which
`process_create` grants only on a job handle that carries
`WRITE_EXECUTE`; a creator passes it on only to a program whose recipe
declares it (DESIGN.md, principle 7). The seed chain's programs, each a
single readable, writable and executable section, are the case it exists
for. The kernel creates the first process without the option and gives
it the root job's handle with the right, so the first process can grant
it and cannot use it. The rule is per mapping: a process that holds a
section with `WRITE` and `EXECUTE` can still write code through one
mapping and run it through another, or change a mapping's protection
with `process_protect`.

Rules:

1. A call that creates an object returns a handle with every right the
   type defines, except that a channel endpoint never carries `DUPLICATE`
   ([ipc.md](ipc.md#i1-channels)), a device section never carries
   `EXECUTE` or `PIN` ([ipc.md](ipc.md#i6-device-memory-and-io-ports)),
   and a job's handle carries `WRITE_EXECUTE` only if the handle of the
   job it was created in did.
2. `handle_duplicate`, `handle_restrict` and a transfer in a channel
   message take a rights mask, or `RIGHTS_SAME` (all ones) to keep the
   source's rights. A mask that is not a subset of the source's rights
   fails with `ERR_ACCESS_DENIED`; nothing is silently dropped.
3. No operation adds a right to a handle. A handle with fewer rights cannot
   be turned back into one with more.

- **Why:** with no ambient authority, a handle is the only authority a
  process has; if rights could grow, any handle would be as good as the
  object's full authority. Narrowing on transfer lets a process pass on
  exactly what a recipient needs, as Zircon ([Rights][zx-rights],
  [zx_handle_duplicate][zx-dup]) and seL4 (seL4 3.1.4) allow.
- **Why an error, not a silent downgrade:** a caller that asks for a right
  it lacks has a bug; seL4 instead gives the new capability the source's
  rights without reporting an error (seL4 3.1.4), which hides it.
- **Cost:** a program cannot regain a right it gave away, even one it once
  had; it must keep a fuller handle or ask the server that issued the
  narrower one.
- **Not taken:** NT's model, in which a handle's access is checked against
  the object's security descriptor when the handle is opened, and in which
  "in some cases, the new handle can have more access rights than the
  original" ([DuplicateHandle][ms-duphandle]). The layout keeps NT's split
  of type-specific rights in the low 16 bits and common rights above them
  ([ACCESS_MASK][ms-accessmask], [Standard Access Rights][ms-stdrights]),
  but there are no generic rights to map and no `MAXIMUM_ALLOWED`, because
  there are no security descriptors to check against. NT's cost, for
  nova-os, is ambient authority: any process that knows a name and passes
  the descriptor's check can open the object
  ([Kernel Objects][ms-kernobj]).

## O6. No ambient authority

**Decided** (DESIGN.md, principle 7).

A process starts with exactly one handle: the bootstrap handle its creator
passes to `process_start`, usually one endpoint of a channel. Every other
handle it ever holds arrives in a message on a channel it can already
read, or is one it created. The kernel installs nothing else: no inherited
handles, no namespace root, no console, no device. A process given no
bootstrap handle has no way to reach any object outside itself.

A process holds no handle to itself unless its creator sends one. A
creator normally includes, in the bootstrap message, a handle to the new
process with `MAP` and `CREATE_THREAD`, so that the process can manage its
own memory and threads; a creator that leaves out `MAP` makes a process
whose address space only the creator can change.

Authority to act on existing things always comes from a handle:

| To do this | A process needs |
|---|---|
| Create a process | A job handle with `CREATE_PROCESS` |
| Create a job | A job handle with `CREATE_JOB` |
| Map device memory, create an interrupt, allow I/O ports, create an initiator | A resource handle with `USE` covering the range or source |
| Write to the kernel's debug log | A system resource handle for the debug log |
| Open a file or find a service by name | A directory handle that reaches it ([O8](#o8-the-namespace)) |

Creating a fresh event, channel, port or zero-filled section needs no
handle: the new object is reachable only through the handle the call
returns, so creating it confers no authority over anything that already
exists. Its kernel memory is charged to the caller's job (**Decided**).

A job's quota limits the handles, objects, queued message bytes and pages
its processes hold. A job's limits are reserved from its parent's when the
job is created, so a job can never be granted more than its parent has
left, and charging is local to one job (**Decided**).

- **Why:** principle 7 has a program's creator pass handles to exactly
  what its derivation declares; that bounds the program only if nothing
  else reaches the process. Zircon states the same property for its
  namespaces: "you can only access objects that are reachable from the
  ones you already have access to. There is no ambient authority"
  ([Namespaces][zx-ns]), and passes one bootstrap handle to a new process
  ([zx_process_start][zx-pstart]).
- **Cost:** every process launch needs a launcher that assembles and sends
  the bootstrap message; there is no default environment to fall back on.
- **Not taken:** NT's handle inheritance, in which every handle created
  inheritable passes to a child created with `bInheritHandles` set
  ([Handle Inheritance][ms-inherit]); a child then holds handles nobody
  chose for it. Zircon's job policy, which gates object-creating calls per
  job ([Kernel concepts][zx-concepts]), is a finer control than a quota
  but a second mechanism; seL4 requires an untyped-memory capability for
  every allocation (seL4 2.4), which accounts for memory exactly at the
  cost of passing allocator capabilities to every component.

## O7. Jobs

**Decided.**

Jobs form a tree rooted at the root job, which the kernel gives to the
first user process. Every process belongs to one job, fixed at creation.
A job holds a quota ([O6](#o6-no-ambient-authority)), and `task_kill` on a
job terminates every process in it and in its descendants. A job is
destroyed when it has no processes, no child jobs and no handles.

- **Why:** a crashed driver must be restarted cleanly (DESIGN.md), which
  means stopping every process it started; a job does that in one call.
  The same tree carries the kernel-memory quota and the authority to
  create processes.
- **Cost:** one more object type, and a tree the kernel walks on
  termination.
- **Not taken:** quotas per process with no tree, set by the creator. Its
  cost is that a supervisor cannot reliably stop processes a child started
  without its knowledge. NT's job objects group processes for limits and
  termination ([Job Objects][ms-jobs]); Zircon's jobs also carry policy
  ([Job][zx-job]).

## O8. The namespace

**Decided.**

nova-os has one namespace: a single tree of names over every named object,
kernel objects and files alike. Names are UTF-8, compared case-sensitively,
with `/` as the separator; the Windows compatibility layer translates NT
and DOS paths (`\`, drive letters, UTF-16) into this form for the programs
that use them. The kernel holds no names. The tree is
served by a user-mode namespace server, and a directory is a channel to a
server that speaks the directory protocol. Every lookup is relative to a
directory handle the caller holds:

1. To open a name, a program creates a channel and sends
   `open(path, rights)` with one of its endpoints on a directory handle.
   The server connects that endpoint to the named object: it serves the
   endpoint itself, passes it to the server that implements the object, or,
   for a kernel object it holds, replies on it with a duplicate handle.
2. A path never climbs above the directory it is resolved against: the
   server rejects `..` at a directory's top, and the client resolves `..`
   textually before sending, as Fuchsia's client library does
   ([Namespaces][zx-ns]).
3. A file system server publishes its root directory under a name. The
   namespace server passes an open below that name, with the rest of the
   path and the client's endpoint, to the file system over its own channel,
   and the file system serves the endpoint from then on, so file traffic
   does not pass through the namespace server.
4. Publishing a name needs a directory handle that carries the protocol's
   publish right.

Holding the root directory handle gives access to the whole tree; a
launcher gives a program the subtrees its recipe names and nothing above
them. Directory rights (list, open for write, publish) belong to the
directory protocol and are enforced by its server; the kernel enforces
only the channel's `READ` and `WRITE`.

- **Why:** O1's test puts names outside the core, since a user-mode server
  provides every lookup a kernel directory would. Serving files and kernel
  objects from one protocol gives one namespace across both, as NT's does
  (`\DosDevices\C:\Directory\File` names a file through the object
  namespace, [Object Names][ms-objnames]). Relative lookup through a
  handle is what makes a namespace compatible with O6. The client supplies
  the endpoint because a channel endpoint has one holder
  ([ipc.md I1](ipc.md#i1-channels)): a server can never hand out copies of
  a channel it holds, so each open makes a new channel.
- **Cost:** a lookup is an IPC round trip to a server, not one call; the
  namespace server must start before anything that resolves names; if it
  fails, lookups stop until it is restarted, though handles already
  obtained keep working.
- **Not taken:** NT's kernel object directories (`\Device`,
  `\KernelObjects`, [Object Directories][ms-objdirs]) with a kernel lookup
  call relative to a directory handle. Its cost is path parsing, name
  storage, symbolic links and permanent objects in the privileged core,
  while file paths would still have to leave the kernel for user-mode file
  systems. NT's global root also lets any process open any name it can
  pass the security check for ([Kernel Objects][ms-kernobj]); Windows
  isolates named objects only by opting in to private namespaces
  ([Object Namespaces][ms-objns]).

## O9. Waiting

**Decided.**

Every waitable object has a 32-bit signal set. Bits 0 to 22 are defined by
the object's type, bit 23 is the synthetic `HANDLE_CLOSED`, and bits 24 to
31 are user signals.

| Type | Signals |
|---|---|
| Process | `TERMINATED` (0) |
| Thread | `TERMINATED` (0) |
| Job | `TERMINATED` (0): terminated and empty |
| Channel | `READABLE` (0), `WRITABLE` (1), `PEER_CLOSED` (2) |
| Event | user signals only |

Sections, ports, resources, interrupts, initiators and pins are not
waitable; a port is read with `port_wait`, and an interrupt delivers port
packets ([ipc.md](ipc.md#i7-interrupts)). In the first set only events have
user signals, set and cleared with `object_signal`.

| Call | Waits for |
|---|---|
| `object_wait_one` | Any of a set of signals on one object, until a deadline |
| `object_wait_many` | Any signal of interest on any of up to `WAIT_MANY_MAX` (64) objects, until a deadline; with no objects, sleeps until the deadline |
| `object_wait_async` | Registers a one-shot watch: when any of the signals is active, one packet goes to a port |

1. A wait observes; it never changes an object's state. An event stays
   signalled until a thread clears it.
2. A wait succeeds as soon as any requested signal is active, including at
   the time of the call. There is no wait-for-all.
3. A deadline is an absolute time on the monotonic clock in nanoseconds;
   `DEADLINE_INFINITE` (all ones) never expires and 0 polls.
4. If a handle being waited on is closed by another thread, the wait ends
   with `ERR_CANCELED` and that item's observed signals include
   `HANDLE_CLOSED`.
5. Results can be stale by the time the caller acts on them; a woken
   reader must tolerate finding a channel already drained by another
   thread, as Zircon warns ([Signals][zx-signals]).

- **Why observe-only:** waits on many objects then have no partial side
  effects, and the result is the same whichever item satisfied the wait.
  Zircon waits work this way ([Signals][zx-signals],
  [zx_object_wait_many][zx-waitmany]). "Wake exactly one thread" comes from
  ports, where each packet goes to one waiter.
- **Why 64:** Zircon's `ZX_WAIT_MANY_MAX_ITEMS` is 64, with ports for more
  ([zx_object_wait_many][zx-waitmany]); NT also caps a multiple-object wait
  at `MAXIMUM_WAIT_OBJECTS` and points to thread pools beyond it
  ([WaitForMultipleObjects][ms-wfmo]). The bound keeps a wait's kernel
  bookkeeping on the kernel stack.
- **Why absolute deadlines:** a wait retried after a wake that did not
  satisfy the caller keeps its original deadline without recomputing it.
- **Cost:** no atomic acquisition of several objects, no auto-reset
  events, and no waits that consume semaphore counts; NT programs that use
  them need a subsystem layer.
- **Not taken:** NT's dispatcher semantics, where a synchronization event
  releases one waiter and resets, and a wait for all objects acquires them
  atomically ([Kernel Dispatcher Objects][ms-dispatch],
  [Wait Functions][ms-waitfns]). Their cost is wait paths that mutate
  objects, per-type acquisition rules in every multi-object wait, and
  abandoned-mutex states.

## O10. Object lifetime

**Decided.**

1. Objects are reference counted. Every handle holds a reference, and so
   do kernel uses: a mapping holds its section, a message in a channel
   holds the handles it carries, a registered watch holds its object and
   port, a bound interrupt holds its port, a pin holds its section's pages,
   and a running thread holds its process. An object is destroyed when its
   last reference goes. NT and Zircon likewise count references and open
   handles separately ([Life Cycle of an Object][ms-lifecycle],
   [Kernel object lifetime][zx-lifetime]).
2. Closing the last handle to an object runs the type's last-handle rule
   even while kernel references remain:

   | Type | When its last handle closes |
   |---|---|
   | Channel endpoint | Queued messages are destroyed, closing the handles in them; the peer asserts `PEER_CLOSED` |
   | Port | Watches and interrupt bindings that name it are cancelled; queued packets are discarded |
   | Interrupt | The source is unbound and masked |
   | Pin | The pin is quarantined unless it was released with `pin_release` ([ipc.md](ipc.md#i8-dma)) |
   | Process, thread | Destroyed if never started; otherwise nothing: they run until they exit or are killed |
   | Initiator | Its quarantined pins stay allocated for the life of the system, since the device may still write them |
   | Event, section, job, resource | Nothing beyond the reference drop |

   A handle carried in a queued message counts as a handle of its object,
   so an endpoint in transit does not trigger its peer's `PEER_CLOSED`.

3. A process terminates when it calls `process_exit`, when its last thread
   exits (exit code 0), when a holder of `TERMINATE` kills it or its job,
   or when one of its threads faults in user mode and no handler resolves
   the fault (exit code from the exception vector). A fault is first sent
   as a message on the process's exception channel, if a holder of
   `TERMINATE` on the process or its job opened one with
   `task_exception_channel`; the receiver, a debugger or a supervisor,
   resumes the thread or lets the process terminate. Termination stops its threads, closes every handle
   in its table, removes its mappings, records the exit code and asserts
   `TERMINATED`. The process object lives on while handles to it exist, so
   its creator can read the code with `process_info`.
4. Destroying an object can close handles to further objects (a channel
   carrying a message carrying a channel, as [Channel][zx-channel] notes).
   The kernel destroys from an explicit work list, never by recursion, so
   the depth of such nesting is bounded only by memory, not by the kernel
   stack.
5. No object outlives its references. NT's permanent objects
   (`OBJ_PERMANENT`, [Life Cycle of an Object][ms-lifecycle]) have no
   counterpart: a name in the namespace keeps an object alive because the
   namespace server holds a handle to it.

- **Why:** reference counting releases resources at a definite point that
  a driver restart or a process exit can rely on, and makes every kernel
  pointer to an object safe for as long as it is held.
- **Cost:** endpoints carried in each other's queues (one endpoint queued
  on a second while the second is queued on the first) form a cycle that,
  once their peers close, nothing can read and nothing frees; the memory
  stays charged to the writers' jobs. The kernel rejects only the direct
  case, a channel's own endpoints in a message written to it
  ([ipc.md I2](ipc.md#i2-handle-transfer)); Zircon rejects the endpoint
  being written ([zx_channel_write][zx-write]).
- **Not taken:** seL4's model, in which objects live in memory retyped
  from untyped capabilities and are destroyed when their last capability
  is deleted or revoked (seL4 2.4, 3.2), with revocation of every derived
  capability. Its cost here is a capability derivation tree in the core;
  nova-os has no revocation, and access a holder might lose is given
  through a server that can cut it off.

## References

Microsoft Learn, with the date each page shows as its last update:
[Kernel Objects][ms-kernobj] (2021-01-07),
[Handle Inheritance][ms-inherit] (2021-01-07),
[DuplicateHandle][ms-duphandle] (2022-08-23),
[ACCESS_MASK][ms-accessmask] (2025-10-15),
[Standard Access Rights][ms-stdrights] (2025-07-10),
[Windows Kernel-Mode Object Manager][ms-objmgr] (2025-05-12),
[Object Names][ms-objnames] (2021-12-15),
[Object Directories][ms-objdirs] (2021-12-15),
[Object Namespaces][ms-objns] (2021-01-07),
[Life Cycle of an Object][ms-lifecycle] (2021-12-15),
[ObReferenceObjectByHandle][ms-obref] (2022-04-18),
[Wait Functions][ms-waitfns] (2021-01-07),
[WaitForMultipleObjects][ms-wfmo] (2024-08-22),
[Introduction to Kernel Dispatcher Objects][ms-dispatch] (2021-12-15),
[Job Objects][ms-jobs] (2025-07-14),
[ZwMapViewOfSection][ms-mapview] (2024-05-22),
[Overview of the Windows I/O Model][ms-iomodel] (2025-02-21).

Fuchsia, Zircon kernel documentation:
[Zircon Kernel Concepts][zx-concepts] (2026-10-12),
[Handles][zx-handles] (2025-02-28),
[Rights][zx-rights] (2025-03-22),
[Signals][zx-signals] (2025-03-22),
[Kernel object lifetime][zx-lifetime] (2026-05-27),
[Channel][zx-channel] (2025-02-28),
[zx_channel_write][zx-write] (2025-03-04),
[zx_handle_duplicate][zx-dup] (2025-03-04),
[zx_handle_replace][zx-replace] (2025-03-04),
[zx_object_wait_many][zx-waitmany] (2026-07-14),
[zx_process_start][zx-pstart] (2025-03-04),
[Job][zx-job] (2025-02-28),
[Timer][zx-timer] (2025-02-28),
[VM Address Region][zx-vmar] (2025-02-28),
[Fuchsia Namespaces][zx-ns] (2026-10-12).

[ms-kernobj]: https://learn.microsoft.com/en-us/windows/win32/sysinfo/kernel-objects
[ms-inherit]: https://learn.microsoft.com/en-us/windows/win32/sysinfo/handle-inheritance
[ms-duphandle]: https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-duplicatehandle
[ms-accessmask]: https://learn.microsoft.com/en-us/windows/win32/secauthz/access-mask
[ms-stdrights]: https://learn.microsoft.com/en-us/windows/win32/secauthz/standard-access-rights
[ms-objmgr]: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/windows-kernel-mode-object-manager
[ms-objnames]: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/object-names
[ms-objdirs]: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/object-directories
[ms-objns]: https://learn.microsoft.com/en-us/windows/win32/sync/object-namespaces
[ms-lifecycle]: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/life-cycle-of-an-object
[ms-obref]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-obreferenceobjectbyhandle
[ms-waitfns]: https://learn.microsoft.com/en-us/windows/win32/sync/wait-functions
[ms-wfmo]: https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects
[ms-dispatch]: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/introduction-to-kernel-dispatcher-objects
[ms-jobs]: https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects
[ms-mapview]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwmapviewofsection
[ms-iomodel]: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/overview-of-the-windows-i-o-model
[zx-concepts]: https://fuchsia.dev/fuchsia-src/concepts/kernel/concepts
[zx-handles]: https://fuchsia.dev/fuchsia-src/concepts/kernel/handles
[zx-rights]: https://fuchsia.dev/fuchsia-src/concepts/kernel/rights
[zx-signals]: https://fuchsia.dev/fuchsia-src/concepts/kernel/signals
[zx-lifetime]: https://fuchsia.dev/fuchsia-src/reference/kernel_objects/objects#kernel-object-lifetime
[zx-channel]: https://fuchsia.dev/fuchsia-src/reference/kernel_objects/channel
[zx-write]: https://fuchsia.dev/fuchsia-src/reference/syscalls/channel_write
[zx-dup]: https://fuchsia.dev/fuchsia-src/reference/syscalls/handle_duplicate
[zx-replace]: https://fuchsia.dev/fuchsia-src/reference/syscalls/handle_replace
[zx-waitmany]: https://fuchsia.dev/fuchsia-src/reference/syscalls/object_wait_many
[zx-pstart]: https://fuchsia.dev/fuchsia-src/reference/syscalls/process_start
[zx-job]: https://fuchsia.dev/fuchsia-src/reference/kernel_objects/job
[zx-timer]: https://fuchsia.dev/fuchsia-src/reference/kernel_objects/timer
[zx-vmar]: https://fuchsia.dev/fuchsia-src/reference/kernel_objects/vm_address_region
[zx-ns]: https://fuchsia.dev/fuchsia-src/concepts/process/namespaces
