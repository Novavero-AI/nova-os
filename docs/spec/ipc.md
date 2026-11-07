# Messages, ports and user-mode drivers

How nova-os processes exchange messages and handles, how a user-mode
driver receives its device's interrupts and reaches its device's memory,
and how an asynchronous I/O request travels from a program to a driver and
back. Object types, handles, rights, signals and lifetime are specified in
[objects.md](objects.md); the calls named here are listed with their
registers in [syscalls.md](syscalls.md).

Specifications are cited by the versions in [README.md](README.md): "SDM"
is the Intel manual's Volume 3 and "SDM Vol. 1" its Volume 1, "VT-d" the
Intel VT-d specification, "AMD IOMMU" the AMD IOMMU specification, and
"ACPI" ACPI 6.6. "seL4 8.1" is section 8.1 of the seL4 Reference Manual,
and "Liedtke 1993", "Liedtke 1995" and "Heiser and Elphinstone 2016" the
papers, at the versions pinned under
[Design references](README.md#design-references). Microsoft Learn and
Fuchsia pages are linked under [References](#references).

## I1. Channels

**Decided:** the core carries messages between processes (DESIGN.md,
principle 2). **Decided:** everything below.

A channel is a pair of endpoints. A message written to one endpoint is
queued at the other, in order. A message is up to `CHANNEL_MAX_BYTES`
(65,536) bytes and up to `CHANNEL_MAX_HANDLES` (64) handles.

1. `channel_create(capacity)` returns both endpoints. `capacity` is the
   number of messages each endpoint's queue holds, from 1 to
   `CHANNEL_MAX_CAPACITY` (1,024); 0 fails with `ERR_INVALID_ARGS` and a
   larger value with `ERR_OUT_OF_RANGE`.
2. `channel_write` copies the bytes into the kernel and moves the handles
   ([I2](#i2-handle-transfer)), all or nothing. If the peer's queue is full
   it fails with `ERR_SHOULD_WAIT` and changes nothing; the writer waits
   for `WRITABLE`. If the peer is closed it fails with `ERR_PEER_CLOSED`.
3. `channel_read` copies the first queued message out and moves its
   handles into the reader's table. If the reader's buffers are too small
   it fails with `ERR_BUFFER_TOO_SMALL`, reports the sizes needed, and
   leaves the message queued. An empty queue gives `ERR_SHOULD_WAIT`, or
   `ERR_PEER_CLOSED` once the peer is closed.
4. An endpoint asserts `READABLE` while its queue is not empty, `WRITABLE`
   while the peer's queue has room, and `PEER_CLOSED` once the peer's last
   handle is closed. Messages already queued stay readable after the peer
   closes.
5. Queued bytes are charged to the writer's job until the message is read
   or destroyed ([objects.md O6](objects.md#o6-no-ambient-authority)).
6. A channel endpoint never carries `DUPLICATE`, so it has one holder at a
   time and its messages have one reader.

- **Why:** a queue lets a request wait at a server while the client
  continues, which NT's asynchronous I/O model needs
  ([I3](#i3-synchronous-call-and-asynchronous-queues)).
  The limits are Zircon's (`ZX_CHANNEL_MAX_MSG_BYTES` 65,536 and
  `ZX_CHANNEL_MAX_MSG_HANDLES` 64, [zx_channel_write][zx-write]); the
  single-holder rule and the buffer-too-small rule are also Zircon's
  ([Channel][zx-channel], [zx_channel_read][zx-read]).
- **Why backpressure as an error:** a bounded queue and an explicit
  `ERR_SHOULD_WAIT` give the writer flow control it can see, and the queue
  depth doubles as the number of requests a client may have outstanding
  ([I9](#i9-asynchronous-io-requests)).
- **Cost:** every writer must handle a full queue. Zircon chose the
  opposite for this reason: its experience is that for rare "try again"
  errors "the application handling code is rarely correct", so it lets
  writes succeed and raises a policy exception in a writer that exceeds a
  hidden limit ([IPC limits][zx-limits]).
- **Not taken:** unbounded queues limited only by kernel memory. Their
  cost is that one slow reader can exhaust the kernel memory every other
  process depends on, the problem Zircon's limits exist to stop.

## I2. Handle transfer

**Decided:** transferring a handle needs `TRANSFER` and can only narrow
its rights ([objects.md O5](objects.md#o5-access-rights)). **Decided:**
the rest.

`channel_write` takes an array of `{handle, rights}` entries. Each handle
must carry `TRANSFER`, and `rights` is `RIGHTS_SAME` or a subset of the
handle's rights. On success each handle leaves the writer's table and
travels in the message with the given rights; on any failure every handle
stays where it was. The array may not name a handle twice, and may not
contain either endpoint of the channel being written (`ERR_NOT_SUPPORTED`).
`channel_read` writes each received handle as `{handle, type, rights}` so
the reader can check what it was given without another call.

If a message is destroyed unread (its receiving endpoint's last handle
closes), the handles it carries are closed.

- **Why move, not copy:** a sender that wants to keep access duplicates
  first, so the default never leaves authority behind by accident. Zircon
  moves handles the same way ([Handles][zx-handles]); seL4 copies them,
  and the sender keeps its capability (seL4 4.2.2).
- **Why all or nothing:** with `ERR_SHOULD_WAIT` a normal outcome, the
  writer must still own its handles to retry.
- **Cost:** a writer that gives up after a failed write still owns the
  handles and must close them.
- **Not taken:** Zircon's rule that a write consumes the handles "on
  success or failure" ([zx_channel_write][zx-write]). Its cost is that a
  writer cannot retry a refused message with the same handles.

## I3. Synchronous call and asynchronous queues

**Decided.**

The core provides asynchronous, bounded channels ([I1](#i1-channels)),
ports to wait on many sources ([I4](#i4-ports)), and one synchronous
convenience, `channel_call`. It provides no rendezvous endpoint.

`channel_call(channel, args, deadline)` writes a request and waits for its
reply in one call. The first four bytes of every message are a
transaction ID. The kernel replaces the request's ID with one it assigns,
with bit 31 set and unique among calls in progress on that endpoint; a
message arriving with that ID goes straight to the waiting caller instead
of the queue. A server replies by writing a message that starts with the
request's ID. If the deadline passes first, the call returns
`ERR_TIMED_OUT` and a late reply is queued normally. Several threads may
call on one channel at once. The design is Zircon's `zx_channel_call`
([zx_channel_call][zx-call]), with these rules:

- A request shorter than four bytes fails with `ERR_INVALID_ARGS`.
- If the write fails, the caller keeps its handles, as with
  `channel_write` ([I2](#i2-handle-transfer)).
- A reply too large for the caller's buffers is discarded, its handles
  closed, and the call fails with `ERR_BUFFER_TOO_SMALL`, as Zircon's
  does; the reply belongs to that call, so it is not left for other
  readers.
- A transaction ID a program assigns itself, outside `channel_call`, has
  bit 31 clear, so it never matches a call in progress.

When a reader is already blocked on an empty queue, the kernel may copy a
message straight to it and switch to it (Liedtke 1993, 5.3.5; Heiser and
Elphinstone 2016, 4.3). That is an implementation choice with no visible
effect on the semantics above.

- **Why:** NT's I/O model is asynchronous: a thread that issues an
  asynchronous request "continues processing another job until the kernel
  signals to the thread that the I/O operation is complete"
  ([Synchronous and Asynchronous I/O][ms-syncasync],
  [Windows I/O model][ms-iomodel]). With user-mode drivers, that request is
  a message, so it must be able to wait in a queue at the driver while
  the client runs. A driver must also wait for client requests and its
  interrupts at the same time; with synchronous IPC alone, L4 found that
  "a single-threaded server could not wait for client requests and
  interrupts at the same time", and that synchronous message passing "is
  clearly the wrong way of synchronising activities across processor
  cores" (Heiser and Elphinstone 2016, 3.2.1). `channel_call` keeps the
  one-system-call round trip that Liedtke's `call` and `reply & receive
  next` gave synchronous clients (Liedtke 1993, 5.2.1).
- **Cost:** each message is copied into a kernel buffer and out again,
  where L4 passes short messages in registers and copies long ones once
  (Liedtke 1993, 5.2.3 and 5.3.6); the kernel must account for and limit
  buffered messages ([I1](#i1-channels)); a server replies with a write
  and then reads its next request, two calls where seL4's `ReplyRecv` is
  one.
- **Not taken:** seL4's model: synchronous rendezvous endpoints that never
  buffer, with notifications for asynchronous signals and reply
  capabilities for call and reply (seL4 4.2, 4.2.4, 5). Its cost for
  nova-os is that an asynchronous request needs either a server thread
  blocked per outstanding request or a request queue built by the
  programs themselves in shared memory, signalled by notifications, with
  handle transfer still going through the kernel separately.

## I4. Ports

**Decided.**

A port is a queue of fixed-size packets that one or more threads wait on
with `port_wait`. Each packet goes to exactly one waiting thread, in the
order queued.

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `key`: chosen by whoever caused the packet |
| 8 | 4 | `type`: `PACKET_USER` (0), `PACKET_SIGNAL` (1), `PACKET_INTERRUPT` (2) |
| 12 | 4 | `status`: 0 for every packet type in the first set |
| 16 | 32 | Payload |

| Type | Source | Payload |
|---|---|---|
| `PACKET_USER` | `port_queue` | 32 bytes from the caller |
| `PACKET_SIGNAL` | A watch registered with `object_wait_async` | Signals asked for (4 bytes), signals active when it fired (4 bytes) |
| `PACKET_INTERRUPT` | An interrupt bound with `interrupt_bind` | Monotonic time of the interrupt in nanoseconds (8 bytes) |

1. `port_create(capacity)` sets how many user packets the port may hold,
   from 1 to `PORT_MAX_CAPACITY` (4,096); `port_queue` on a full port fails
   with `ERR_SHOULD_WAIT`.
2. A watch is one-shot: it queues one packet when any requested signal is
   active, at registration or later, and then ends. A server re-registers
   after draining the object.
3. The packet for each watch and each bound interrupt is allocated when the
   watch is registered or the interrupt bound, so delivering it never
   fails for lack of memory. A registration that cannot be charged to the
   caller's job fails with `ERR_NO_MEMORY`. This kernel does not charge
   kernel memory to jobs ([objects.md O6](objects.md#o6-no-ambient-authority));
   in the quota's place a port takes at most `PORT_WATCHES_MAX`
   watches, registered or with a packet queued, one for each handle a
   process can hold (`kernel/object/port.h`), and a registration past
   them fails with `ERR_NO_MEMORY` too.
4. Closing the watched handle ends its watch; packets already queued stay.
5. A thread blocked in `port_wait` whose port handle closes, or leaves
   its table in a message, wakes with `ERR_CANCELED`, as a wait on a
   closed handle does ([objects.md O9](objects.md#o9-waiting)).

- **Why:** a port is the one place a single thread waits for client
  requests, interrupts and completions together, the need Heiser and
  Elphinstone describe (2016, 3.2.1). It plays the role of NT's I/O
  completion port, which combines "the synchronization point for multiple
  file handles into a single object" and lets other threads post their
  own packets ([I/O Completion Ports][ms-iocp]), and of Zircon's port
  ([Port][zx-port], [zx_object_wait_async][zx-waitasync]). One packet per
  waiter gives the wake-exactly-one behaviour that NT's synchronization
  events provide ([objects.md O9](objects.md#o9-waiting)).
- **Cost:** one-shot watches cost one system call per re-arm.
- **Not taken:** persistent watches that queue a packet on every signal
  change; their cost is packet floods from busy objects and unbounded
  queue growth. NT's concurrency value, which limits how many threads
  associated with a completion port run at once ([I/O Completion
  Ports][ms-iocp]), needs scheduler cooperation and is left out of the
  first set.

## I5. Bulk data

**Decided.**

Channels carry requests, replies and handles. Bulk data moves through
sections both sides map: one process creates a section, sends a handle to
it with the rights the other side needs, and both map it. Reads and writes
then name offsets within the section.

- **Why:** copying through the kernel twice per message suits control
  traffic, not data. L4's experience is that shared buffers "can avoid any
  explicit copying between address spaces and are generally preferred for
  bulk data transfer", which is why L4 dropped its in-kernel long-message
  copy (Heiser and Elphinstone 2016, 3.2.2).
- **Cost:** the sharing process exposes the section's contents to its peer
  for as long as both map it, and must treat what the peer writes there as
  untrusted until a reply says it is complete.
- **Not taken:** long messages that the kernel copies straight into the
  receiver through a temporary mapping, as the original L4 did (Liedtke
  1993, 5.2.3). L4 removed them because page faults during the copy made
  the kernel handle nested exceptions (Heiser and Elphinstone 2016,
  3.2.2).

## I6. Device memory and I/O ports

**Decided:** drivers are user-mode processes holding handles to the devices
and memory they serve (DESIGN.md, principle 2). **Decided:** the
mechanism.

At boot the kernel creates one root resource of each kind and passes them,
with the root job, to the first user process in its bootstrap message:

| Kind | Range |
|---|---|
| `RESOURCE_MMIO` | All physical addresses outside RAM ([boot-protocol.md](boot-protocol.md#memory)), except the registers of devices the kernel drives itself: the local APIC and the range FEE0_0000h to FEEF_FFFFh that message-signalled interrupts are written to (SDM 13.11.1), the I/O APICs (ACPI 5.2.12), and any IOMMU (VT-d 8.3, AMD IOMMU 5.2.2.1). Its range is 0 to 2 to the power of the physical-address width (SDM 5.1.4); `section_create_physical` keeps RAM and those registers out |
| `RESOURCE_IRQ` | All interrupt sources: ACPI global system interrupts (ACPI 5.2.13) and message-signalled vectors. Global system interrupt g is source g, and the message-signalled interrupts of requester ID r ([I8](#i8-dma)) are source 2^32 + r |
| `RESOURCE_IOPORT` | I/O ports 0 to 65,535, except those of the serial port the kernel logs to (COM1, from 0x3F8) |
| `RESOURCE_DMA` | All bus requester IDs ([I8](#i8-dma)), 0 to 2^32 |
| `RESOURCE_SYSTEM` | Named privileges; in the first set only the debug log |

The first process passes them to a platform driver, which enumerates ACPI
and PCI in user mode and gives each device driver resources covering only
its device; in the first set the first process is the platform driver.
`resource_create` derives a resource covering a subrange of its parent's;
a resource's range never changes.

The message's bytes also describe the framebuffer nova-boot reports in
`BOOT_INFO`, which the root MMIO resource covers; once the kernel has
released the display, the first process gives a resource covering only
the framebuffer to the compositor while one runs, and to the console
otherwise ([console.md C1](console.md#c1-the-display-handoff),
[display.md D2](display.md#d2-who-holds-the-display)).

The kernel reads the ACPI tables that name registers it keeps (the MADT
for the I/O APICs, the DMAR and IVRS for IOMMUs) and the MCFG, and passes
the PCI Express configuration space ranges the MCFG describes in the
bootstrap message's bytes, so that the platform driver can enumerate PCI
before it reads ACPI itself. The MCFG's layout is the PCI Firmware
Specification's, which is not pinned
([README.md](README.md#pci-express-configuration-space)). `native_abi.h`
gives the message's handles in order and its bytes.

**Device memory.** `section_create_physical(resource, base, size, cache)`
makes a section over physical addresses that an MMIO resource covers. It
fails if the range overlaps any memory the memory map describes as RAM or
as preserved, so a device section can never alias memory the kernel
allocates: any descriptor other than `EfiMemoryMappedIO` and
`EfiMemoryMappedIOPortSpace`. The one exception is configuration space the
MCFG describes, which firmware reserves in the map
(`EfiReservedMemoryType`, as EDK II does) so that no OS takes it for RAM;
the kernel accepts an MCFG range only where it overlaps nothing else in
the map. It also fails over the registers the kernel keeps: the local
APIC's page, the MSI range, the I/O APICs and the IOMMUs above. `cache` is
uncached (`UC`), which the SDM recommends for memory-mapped I/O (SDM
14.3), or write-combining (`WC`) for framebuffers; the kernel programs it
through the page attribute table (SDM 14.12). A device section's handle
carries `READ` and `WRITE` but never `EXECUTE` or `PIN`, so device memory
is never executed or used as a DMA buffer.

A physical page has one memory type in every mapping of it, since the
SDM does not support a page "mapped to two or more different linear
addresses, each with different memory types", and "a WC page must never
be aliased to a cacheable page because WC writes may not check the
processor caches" (SDM 14.12.4). Of the pages a device section may
cover, the direct map maps only the framebuffer's, write-combining:
nova-boot leaves memory-mapped I/O out of it
([boot-protocol.md](boot-protocol.md#paging)), and the kernel removes
each configuration space range it accepts from the MCFG, then flushes
the caches, which may hold lines read ahead through the old mapping if
the MTRRs gave it a cacheable type: "changing a cache line's memory type
from WB to UC to be used for memory-mapped I/O" needs that flush (SDM
14.12.4). A device section's `cache` then gives its pages their type:
`section_create_physical` fails with `ERR_BAD_STATE` over a framebuffer
page with `UC`, and over a page that a live device section covers with
the other type, so a page keeps the type of its first section until no
section over it lives. Mappings hold their section, so none outlives the
type it was made with.

**I/O ports.** `ioport_set(resource, base, count, allow)` allows or
denies the calling process `IN` and `OUT` on ports an I/O-port resource
covers. The I/O permission bitmap lies inside the TSS, at an offset from
its base (SDM 10.7, "I/O map base address"), so the kernel keeps a bitmap
for each process that has allowed ports and copies it into the CPU's TSS
when it switches to that process. IOPL stays 0, so user code reaches only
allowed ports (SDM 2.3, IOPL): when CPL is greater than IOPL, the
processor checks one bit per port and raises #GP if the bit of any port
an access covers is set. Ports past the bitmap's end count as set, and the
bitmap is followed by a byte with every bit set (SDM Vol. 1 20.5.2). A
process with no allowed ports runs with the TSS's I/O map base at or past
the TSS limit, so there is no I/O permission map and every port access at
CPL 3 raises #GP (the same section). Like a mapping, an allowance outlasts
the resource handle it was made with: it ends when the process denies it
or exits.

- **Why:** the authority to touch a device is then a handle like any
  other, delegated down a chain the platform driver controls. Zircon hands
  "an initial resource of each kind" to the first user process
  ([Resource][zx-resource]) and requires an MMIO resource for a physical
  VMO ([zx_vmo_create_physical][zx-vmophys]); seL4 gives the initial thread
  device untyped memory, which can only become frames (seL4 2.4), and an
  I/O port control capability (seL4 8.2.2).
- **Cost:** the platform driver is trusted with every device; a bug there
  can grant one driver another's registers. Mappings and I/O-port
  allowances persist after their resource handle is sent away, so
  delegating a resource does not withdraw what the sender already set
  up.
- **Not taken, I/O ports:** seL4's per-access calls (`seL4_X86_IOPort_In8`
  and the rest, seL4 8.2.2). Their cost is a system call for every port
  access, against a bitmap per process that uses ports and a TSS update
  when switching to one.

## I7. Interrupts

**Decided:** interrupts reach user-mode drivers (DESIGN.md, principle 2).
**Decided:** the mechanism.

1. `interrupt_create(resource, source, options)` makes an interrupt object
   for an ACPI global system interrupt the IRQ resource covers, with its
   trigger mode and polarity in `options`, or for a message-signalled
   interrupt. For an MSI the kernel allocates a vector, charged to the
   calling process's job until the interrupt's last handle closes
   ([objects.md O6](objects.md#o6-no-ambient-authority)), and returns the
   message address and data, which the bus driver writes into the device's
   MSI capability (SDM 13.11).
2. `interrupt_bind(interrupt, port, key)` delivers the interrupt to one
   port. An interrupt is bound to at most one port.
3. When the interrupt fires, the kernel masks a level-triggered source at
   its interrupt controller, writes the local APIC's EOI register (SDM
   13.8.5), and queues one `PACKET_INTERRUPT` to the port.
4. While a packet is outstanding the kernel queues no other for that
   interrupt; a further edge or message is remembered as pending.
5. After servicing the device, the driver calls `interrupt_ack`. The
   kernel unmasks a level-triggered source, and queues the next packet at
   once if one is pending.
6. Closing the interrupt's last handle unbinds and masks the source.

The local APIC timer and the interrupt controllers stay in the kernel:
the scheduler needs the timer, and only the kernel can route interrupts to
user-mode drivers safely, the two drivers Heiser and Elphinstone note "are
still best kept in the kernel" (2016, 3.3).

- **Why:** a packet on the driver's port lets one driver thread wait for
  its device and its clients together ([I4](#i4-ports)). Masking until the
  acknowledgement keeps a level-triggered line from re-interrupting before
  a user-mode driver has had a chance to run, while the EOI lets every
  other interrupt through meanwhile. Zircon's port-bound interrupts deliver
  no new packet "until the interrupt has been acknowledged"
  ([Interrupts][zx-irq]); in seL4 the driver's `IRQHandler_Ack` likewise
  tells the kernel it has finished, and only then are further interrupts
  delivered (seL4 8.1). Windows has run interrupt handling in user-mode
  drivers since UMDF 1.11 ([Handling Interrupts][ms-umdfirq]).
- **Cost:** each interrupt costs a switch to the driver and a system call
  to acknowledge it; a driver that never acknowledges silences only its
  own device.
- **Not taken:** Liedtke's model, in which "the hardware is regarded as a
  set of threads" that send empty messages to the driver, and the kernel
  releases the interrupt when the driver issues its next IPC (Liedtke
  1995, 2.2). L4 moved away from it because "delivery as messages required
  the emulation of virtual in-kernel threads as the sources of interrupt
  IPC" (Heiser and Elphinstone 2016, 3.3). Zircon also allows a thread to
  block on an interrupt object directly ([Interrupts][zx-irq]); that second
  path is left out because a port with one bound interrupt does the same.

The first set delivers message-signalled interrupts (MSI and MSI-X), which
virtio's PCI devices use (VIRTIO 1.4, PCI transport); a level-triggered
line shared by several devices is left out until a device needs one.

An IRQ resource covers interrupt sources as `RESOURCE_IRQ`'s row in
[I6](#i6-device-memory-and-io-ports) numbers them, so a resource covering
2^32 + r grants the message-signalled interrupts of one requester ID. Each
`interrupt_create` for such a source takes a vector of its own, `options`
0. The kernel cannot mask a message-signalled interrupt at its device,
whose MSI or MSI-X registers the bus driver writes; closing its last
handle frees the vector, and a message the device still sends there
reaches whichever interrupt takes the vector next, as a spurious packet.

A message-signalled interrupt is a memory write to an architectural
address range, so without interrupt remapping any device that can do DMA
can raise any interrupt with any attributes (VT-d 2.5.2.1). Where the
IOMMU remaps interrupts (VT-d chapter 5; AMD IOMMU 2.2.5), the kernel
enables it and gives each device only the entries `interrupt_create` set
up for it. Without remapping, the kernel reports the device's interrupts
as unconfined, as it reports unconfined DMA ([I8](#i8-dma)).

## I8. DMA

**Decided:** DMA is confined only where an IOMMU confines it (DESIGN.md,
Decisions). **Decided:** the mechanism.

1. `initiator_create(resource, requester_id)` makes an initiator object for
   one bus requester ID that a DMA resource covers. For PCI the ID is 32
   bits: the segment group in the high 16, and the bus, device and
   function number in the low 16 (seL4 8.2.3 describes the low half).
   Each IOMMU serves one segment group and indexes its tables by the low
   half (VT-d 8.3 and 3.4.1, AMD IOMMU 5.2.2.1 and 2.2.2). The bus driver
   holds the initiator and passes it to the device's driver.
2. `section_pin(initiator, section, offset, size, options, addresses)`
   pins pages of a section for the device and writes one device address
   per page into `addresses`. `options` says whether the device reads,
   writes or both; the section handle needs `PIN` and the matching `READ`
   or `WRITE`. It returns a pin handle.
3. `pin_release(pin)` ends the pin and closes the handle: the driver
   asserts that the device no longer uses the pages.
4. If a pin's last handle goes any other way (`handle_close`, or the
   driver's exit), the pin is quarantined: its pages stay allocated, held
   by the initiator, until a holder of the initiator's `RELEASE` right
   calls `initiator_release_quarantine` after resetting the device. If the
   initiator's last handle closes with pins still quarantined, their pages
   stay allocated for the life of the system: the device may still write
   them, and nobody is left to reset it.
5. Without an IOMMU, device addresses are physical addresses, and a
   driver that can start DMA can reach all memory; the kernel logs each
   initiator it creates as unconfined. With one, the kernel maps the
   pinned pages into the initiator's IOMMU domain, returns I/O virtual
   addresses, and unmaps them when the pin ends.

With an IOMMU, confinement rests on three things its specification
defines.

**Remapping tables.** The IOMMU looks up each request's requester ID in a
table that names the device's domain and the domain's I/O page tables: on
Intel the root and context tables (VT-d 3.4.2, or 3.4.3 in scalable mode)
over second-stage page tables (VT-d 3.7), on AMD the Device Table (AMD
IOMMU 2.2.2) over I/O page tables (AMD IOMMU 2.2.3). Requester IDs with
no initiator must be blocked as well (VT-d 3.2). VT-d blocks a request
whose root or context entry is not present (VT-d 7.1.1, 7.1.3), but an
AMD Device Table entry whose valid bit is clear passes every address
through untranslated (AMD IOMMU 2.2.2.1, Table 8), so every Device Table
entry needs its valid bit set.

**Device scope.** Firmware's ACPI tables say which IOMMU serves which
requester IDs: the DMAR table's remapping hardware unit structures and
their device scopes (VT-d 8.3, 8.3.1), and the IVRS table's IVHD blocks
and their device entries (AMD IOMMU 5.2.2.1). Requester IDs and devices
are not always one to one: devices behind a conventional PCI bridge carry
the bridge's ID and can only be assigned to a domain together (VT-d
3.12.2), a device using phantom functions issues several IDs whose
entries must match (VT-d 3.12.5, AMD IOMMU 2.2.2), and IVRS alias entries
name the ID a device's requests carry (AMD IOMMU 5.2.2.1). The same
tables also list memory that particular devices must keep reaching,
identity-mapped for them (VT-d 3.16 and 8.4, AMD IOMMU 5.2.2.2).

**Invalidation.** The IOMMU caches translations, so ending a pin clears
its page-table entries, invalidates the cached translations and waits for
the invalidation to complete before the pages are freed. On Intel this is
an IOTLB invalidation of the pages, which also drains requests to them
that the IOMMU has already processed, then an invalidation wait
descriptor (VT-d 6.5.2.3, 6.5.2.9, 6.5.4), through the queued
invalidation interface, the only one hardware past major version 5
accepts (VT-d 6.5). On AMD it is `INVALIDATE_IOMMU_PAGES` then
`COMPLETION_WAIT`, after which no transaction in flight reads or writes
the pages (AMD IOMMU 2.4.3, 2.4.1). Changing a present context entry or a
valid Device Table entry needs its own invalidation too (VT-d 6.5.3.3,
Table 28; AMD IOMMU 2.2.2.2). An Intel IOMMU that reports caching mode,
as emulated ones may, needs an invalidation after every change, a new
pin's mappings included (VT-d 6.1).

Device writes to the interrupt address range are interrupt requests,
which bypass the page tables (VT-d 3.15, AMD IOMMU 2.1.4.2); only
interrupt remapping governs them (VT-d 5.1.4, AMD IOMMU 2.2.2.1,
Table 9).

- **Why:** a device keeps writing to memory after the driver that started
  it has died, so freeing pinned pages when the driver exits would hand
  memory still under DMA to the next allocation. Zircon quarantines
  unreleased pins for this reason ([Bus Transaction Initiator][zx-bti],
  [zx_bti_pin][zx-btipin]).
- **Cost:** pages leak until the device is reset and the quarantine
  released; until IOMMU support exists, a DMA-capable driver is trusted
  with all memory.
- **Not taken:** seL4's model, which exposes the IOMMU's page tables as
  `IOSpace` and `IOPageTable` objects that the driver maps frames into
  (seL4 8.2.3). Its cost is IOMMU page-table management in every driver
  and two more object types, for control of I/O address layout that no
  first-stage driver needs.

Where no IOMMU confines a DMA-capable driver, the kernel still runs it and
reports it as unconfined, so the gap is visible rather than assumed away.

## I9. Asynchronous I/O requests

**Decided.** The message contents below define the mechanism; their exact
layout belongs to the I/O protocol's own specification.

An I/O request is a message on a channel from a client to the server that
implements the file or device. Opening a file connects a new channel
from the client to the file's server, one per open, as NT gives every `CreateFile` a new
file object ([Kernel Objects][ms-kernobj]). Each request has a transaction
ID, and the server answers each one with exactly one completion message
carrying the same ID.

| Request field | Meaning |
|---|---|
| Transaction ID | Assigned by the client, or by the kernel in `channel_call` |
| Operation | Read, write, control, flush, cancel |
| Offset, length | Where in the file or device |
| Buffer | Inline (data in the message) or an offset in the shared section |
| Flags | Operation-specific |

| Completion field | Meaning |
|---|---|
| Transaction ID | The request's |
| Status | A status code as defined in [syscalls.md](syscalls.md#s7-error-reporting) |
| Information | Bytes transferred, or an operation-specific result |
| Data | For an inline read, the bytes read |

A read from a disk, start to finish:

1. Earlier, the client opened the file through a directory handle
   ([objects.md O8](objects.md#o8-the-namespace)), keeping one endpoint of
   a channel whose other endpoint the file system server now serves, and
   sent a section handle for its buffers. It
   registered a watch on the channel with its port, keyed by the file
   ([I4](#i4-ports)).
2. The client writes a read request naming an offset in the shared
   section and goes on with other work. If it already has as many
   requests outstanding as the channel's capacity, the write fails with
   `ERR_SHOULD_WAIT` and it waits for `WRITABLE`.
3. The file system's port delivers a signal packet for the client's
   channel. The server reads the request, maps the file range to blocks,
   and writes block requests on its own channel to the storage driver,
   naming offsets in a section it shares with the driver. It re-arms its
   watch.
4. The storage driver reads the block requests, pins the target pages
   through its initiator ([I8](#i8-dma)), and queues the transfer to the
   device.
5. The device completes and interrupts. The kernel queues an interrupt
   packet to the driver's port ([I7](#i7-interrupts)); the driver reads
   the device's completion, acknowledges the interrupt, releases the pin,
   and writes a completion message to the file system.
6. The file system copies the data from its section into the client's
   section and writes the client's completion message.
7. The client's port delivers a signal packet for the file's channel; the
   client reads the completion and finds the data in its section.

A client that wants to block calls `channel_call` with the request; this
is NT's synchronous I/O, where the thread "enters a wait state until the
I/O request has completed" ([Synchronous and Asynchronous I/O][ms-syncasync]).

**Cancellation.** The client sends a cancel request naming the
transaction ID. The server completes the original request, with
`ERR_CANCELED` if it stopped it in time and normally otherwise; either
way the client receives exactly one completion. A server that sees
`PEER_CLOSED` on a client's channel abandons that client's requests;
pages a device is still writing stay allocated because the pins hold them
([I8](#i8-dma)).

| NT | nova-os |
|---|---|
| An IRP sent to a device object with `IoCallDriver` ([IRPs][ms-irps]) | A request message written to the server's channel |
| One IRP passed down a device stack, with a stack location per driver ([IRPs][ms-irps], [Example I/O Request][ms-example]) | A message per hop, each layer a separate process with its own channel to the next |
| Completion with a status in the IRP's `IO_STATUS_BLOCK` ([Using NTSTATUS Values][ms-ntstatus]) | A completion message with a status and information |
| Buffered I/O: the I/O manager copies through a system buffer ([Methods for Accessing Data Buffers][ms-buffers]) | Inline data in the message |
| Direct I/O: the buffer's pages locked and described by an MDL | A shared section, pinned for DMA |
| Neither buffered nor direct: the driver uses the caller's address in the caller's thread | None: drivers run in their own address spaces |
| I/O completion port | A port with a watch per file channel |
| `IoCancelIrp`; a cancelled IRP not completed in 5 minutes is timed out ([Canceling IRPs][ms-cancel]) | A cancel message; no kernel timeout, the client's deadline governs |

- **Why:** every hop is an ordinary message between processes, so a
  driver or file system is a process like any other, and the same port
  serves a client's files, a server's clients and a driver's interrupts.
  Inline data covers small transfers as NT's buffered I/O does; the
  shared section covers large ones as direct I/O does, and lets a device
  transfer straight into a section the next layer maps, with no copy
  through the kernel.
- **Cost:** each layer adds a message copy and a context switch per
  request that NT's single IRP avoids; a file system and its clients must
  agree on the I/O protocol, which the kernel does not check.
- **Not taken:** an I/O manager in the kernel that builds and routes
  requests, as NT's does ([Windows I/O model][ms-iomodel]). Its cost is
  request routing, buffering policy and device-stack state in the
  privileged core ([objects.md O1](objects.md#o1-admission-to-the-core)).

## I10. Driver failure and restart

**Decided** that a failed driver is restarted instead of stopping the
system (DESIGN.md, principle 2). **Decided:** how.

A supervisor starts each driver in a job of its own, whose limits grant
the interrupt vectors the driver needs and no more, and keeps the
driver's resources, initiator and job handles. It waits for all its
drivers on one port: before it starts a driver it registers a watch of
the driver's process for `TERMINATED` ([I4](#i4-ports)), so one thread
hears of every end whenever it comes, at startup or long after. The
first process is that supervisor for the drivers it starts, for the
life of the system ([input.md K4](input.md#k4-supervision)). When the
driver's process asserts `TERMINATED`:

1. The kernel has already closed the driver's handles: its interrupts are
   masked, its pins quarantined, its channels closed.
2. Each client sees `PEER_CLOSED` on its channel; its outstanding requests
   are lost, and its library reports them failed.
3. The supervisor resets the device, releases the initiator's quarantine,
   starts a new driver with the same resources, and publishes it again in
   the namespace.
4. Clients open the device again by name. A device not yet published by
   name gets its new endpoint to its clients through the supervisor, on
   the channel each client keeps to it, as the input driver's client does
   ([input.md K3](input.md#k3-the-report-and-the-client)).

The supervisor first clears the function's Bus Master Enable, so it
starts no DMA while the reset runs. A virtio device's reset is a write
of 0 to `device_status` in its common configuration structure: "The
device MUST reset when 0 is written to device_status, and present a 0 in
device_status once that is done" (VIRTIO 1.4 4.1.4.3.1). From then "A
device MUST NOT send notifications or interact with the queues" until a
driver initializes it again (2.4.1), which is what releasing the
quarantine needs: a driver "MUST ensure a virtqueue isn't live (by device
reset) before removing exposed buffers" (3.3.1). A device that does not
finish its reset keeps its quarantine and gets no new driver.

**Restart policy.** A driver that ends, or does not start, is started
again unless it was restarted within the last minute; then the
supervisor gives it up, its function stays stopped, and the system runs
on without it. A driver that fails on every start therefore runs twice.
The bound is kept per driver, and holds for a client the supervisor
starts too (`RESTART_PERIOD_SECONDS` in `init/platform.c`). A driver
that ends because its work is done, as the block driver does once it
has read the disk, is not started again; one that ends because the
client it served failed and was restarted starts again without
counting a restart of its own, since the client's bound holds that.

- **Why:** everything the dead driver held is released by the kernel's
  ordinary lifetime rules ([objects.md O10](objects.md#o10-object-lifetime));
  the supervisor needs no cooperation from it. A failure that comes back
  within a minute of a restart is one a restart does not cure, and
  starting the driver again would repeat it as fast as the driver can
  fail, resetting the device each time; a minute is far longer than a
  driver takes to start and fail, under a second even emulated, and
  short enough that a fault that comes back only now and then is
  restarted each time.
- **Cost:** in-flight requests fail and clients must reopen; a driver that
  fails while its device is mid-transfer depends on the supervisor's reset
  for the device to be usable again. A driver whose fault recurs within a
  minute is lost until the next boot, even if a third start would have
  held.
- **Not taken:** a supervisor that holds the client channels itself and
  replays outstanding requests to the restarted driver. Its cost is a
  supervisor that tracks every request in flight, and replayed requests
  that may already have reached the device. Restarting with a growing
  delay and never giving up: its cost is a driver that can never work
  restarted for the life of the system, its device reset at each start,
  and a supervisor that keeps a timer per driver.

## References

Microsoft Learn, with the date each page shows as its last update:
[Kernel Objects][ms-kernobj] (2021-01-07),
[Overview of the Windows I/O Model][ms-iomodel] (2025-02-21),
[Synchronous and Asynchronous I/O][ms-syncasync] (2025-07-08),
[I/O Completion Ports][ms-iocp] (2026-07-17),
[I/O request packets][ms-irps] (2024-09-27),
[Example I/O Request - An Overview][ms-example] (2021-12-15),
[Using NTSTATUS Values][ms-ntstatus] (2025-11-05),
[Methods for Accessing Data Buffers][ms-buffers] (2021-12-15),
[Canceling IRPs][ms-cancel] (2023-11-21),
[Handling Interrupts (UMDF)][ms-umdfirq] (2021-12-15),
[Should I create a generation 1 or 2 virtual machine in Hyper-V?][ms-gen2]
(pinned in [README.md](README.md)).

Fuchsia, Zircon kernel documentation:
[Handles][zx-handles] (2025-02-28),
[Channel][zx-channel] (2025-02-28),
[zx_channel_write][zx-write] (2025-03-04),
[zx_channel_read][zx-read] (2025-03-04),
[zx_channel_call][zx-call] (2025-03-04),
[Zircon Kernel IPC Limits][zx-limits] (2025-02-28),
[Port][zx-port] (2025-02-28),
[zx_object_wait_async][zx-waitasync] (2025-03-04),
[Interrupts][zx-irq] (2026-03-25),
[Resource][zx-resource] (2026-10-20),
[zx_vmo_create_physical][zx-vmophys] (2026-01-08),
[Bus Transaction Initiator][zx-bti] (2025-02-28),
[zx_bti_pin][zx-btipin] (2025-03-04).

[ms-kernobj]: https://learn.microsoft.com/en-us/windows/win32/sysinfo/kernel-objects
[ms-iomodel]: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/overview-of-the-windows-i-o-model
[ms-syncasync]: https://learn.microsoft.com/en-us/windows/win32/fileio/synchronous-and-asynchronous-i-o
[ms-iocp]: https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports
[ms-irps]: https://learn.microsoft.com/en-us/windows-hardware/drivers/gettingstarted/i-o-request-packets
[ms-example]: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/example-i-o-request---an-overview
[ms-ntstatus]: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-ntstatus-values
[ms-buffers]: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/methods-for-accessing-data-buffers
[ms-cancel]: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/canceling-irps
[ms-umdfirq]: https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/handling-interrupts
[ms-gen2]: https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/plan/should-i-create-a-generation-1-or-2-virtual-machine-in-hyper-v
[zx-handles]: https://fuchsia.dev/fuchsia-src/concepts/kernel/handles
[zx-channel]: https://fuchsia.dev/fuchsia-src/reference/kernel_objects/channel
[zx-write]: https://fuchsia.dev/fuchsia-src/reference/syscalls/channel_write
[zx-read]: https://fuchsia.dev/fuchsia-src/reference/syscalls/channel_read
[zx-call]: https://fuchsia.dev/fuchsia-src/reference/syscalls/channel_call
[zx-limits]: https://fuchsia.dev/fuchsia-src/concepts/kernel/ipc_limits
[zx-port]: https://fuchsia.dev/fuchsia-src/reference/kernel_objects/port
[zx-waitasync]: https://fuchsia.dev/fuchsia-src/reference/syscalls/object_wait_async
[zx-irq]: https://fuchsia.dev/fuchsia-src/reference/kernel_objects/interrupts
[zx-resource]: https://fuchsia.dev/fuchsia-src/reference/kernel_objects/resource
[zx-vmophys]: https://fuchsia.dev/fuchsia-src/reference/syscalls/vmo_create_physical
[zx-bti]: https://fuchsia.dev/fuchsia-src/reference/kernel_objects/bus_transaction_initiator
[zx-btipin]: https://fuchsia.dev/fuchsia-src/reference/syscalls/bti_pin
