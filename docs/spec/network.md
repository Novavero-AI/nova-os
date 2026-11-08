# The network

How nova-os reaches a network: the virtio network driver, the frames it
exchanges with the network service, the service's TCP/IP stack and the
resolver beside it, sockets as channels and the grants that bound them,
how network effects meet record and replay, what a compromised part or a
hostile peer can do and the limits on clients, and the order in which to
build it, with what CI checks at each step.
[objects.md](objects.md) decides that authority comes only from handles,
[ipc.md](ipc.md) the driver model and the I/O request,
[storage.md](storage.md) per-process views and limits on clients,
[replay.md](replay.md) what a recording captures, and
[accounts.md](accounts.md) the session; this document builds on all of
them.

DESIGN.md, principle 2, makes networking user-mode processes; that much
is **Decided** there, and every item below is **Decided**.

Each item gives the design, why, what it costs, and the main
alternative not taken with that alternative's cost. Specifications are
cited by the versions in [README.md](README.md): "VIRTIO 1.4 5.1.9" is a
section of VIRTIO 1.4, and "RFC 9293 3.4.1" a section of an RFC pinned
under [Networking](README.md#networking). RFC 768, 792, 826 and 894 have
no numbered sections and are cited by their headings. Labels such as
MUST-13 are RFC 9293's own numbering of its requirements. Other stacks,
QEMU and libslirp are pinned under [Network design
references](README.md#network-design-references) and cited by name.

## N1. The parts

**Decided.**

| Part | What it is | What it holds |
|---|---|---|
| The virtio network driver, `\nova\virtnet.exe` | A user-mode driver for the first virtio network function ([N2](#n2-the-virtio-network-driver)) | What every PCI device driver gets (`drivers/pci.h`): resources covering its function's configuration space, BARs and message-signalled interrupts, and an initiator with `PIN`; the debug log; the frame channel to the service, which it creates |
| The network service, `net.exe` | The TCP/IP stack and the DHCP client; it serves grants and sockets ([N7](#n7-what-v1-implements)) | The frame channel's client end, handed over by the first process; the bootstrap channel it keeps to the first process; the channel to the resolver; the system's settings, to read its own ([N12](#n12-grants)); its own process with `MAP`; the debug log. No directory, no store channel, no job |
| The resolver, `resolver.exe` | The DNS stub resolver ([N11](#n11-names-and-the-resolver)) | The channel the service sends it names on; a grant, from the first process, to the configured name servers' port 53 and nothing else; its own process with `MAP`; the debug log |
| The first process | Starts and supervises the three ([N5](#n5-supervision)) and holds the service's control channel, the root of network authority ([N12](#n12-grants)) | |

The driver is a boot file, as the other drivers are. The service and
the resolver come from the `nova-network` store path, as the first
process starts the accounts service and login from `nova-session`
([accounts.md A1](accounts.md#a1-the-parts)).

- **Why:** each part holds only what its work needs. The driver
  holds the device, whose DMA without an IOMMU reaches all memory
  ([ipc.md I8](ipc.md#i8-dma)), and parses nothing from the network but
  the device's ring entries. The service parses every frame from the
  network and holds every socket, but no device. The resolver parses
  answers from name servers it does not control and holds nothing a
  program gave it. Splitting them puts the most exposed parser, the
  stack's, out of reach of the device, as
  [accounts.md A1](accounts.md#a1-the-parts) keeps the program that
  parses what a stranger types away from every user's files.
- **Cost:** three processes and two protocols where a monolithic stack
  has one; every frame crosses a process boundary twice, in and out.
- **Not taken:** the stack in the driver's process. Its cost is that a
  bug in the code that parses every frame from the network would hold
  the device's DMA authority. The stack in the kernel, as NT keeps
  `tcpip.sys`: [objects.md O1](objects.md#o1-admission-to-the-core)
  admits nothing to the core that a user-mode server can provide.

## The driver

### N2. The virtio network driver

**Decided.** The platform driver starts `\nova\virtnet.exe` for the
first virtio network function it finds, PCI 1AF4:1041 or the
transitional 1AF4:1000 (VIRTIO 1.4 4.1.2, 5.1.1), in a job of its own,
with the bootstrap message every PCI device driver gets
(`drivers/pci.h`). Drivers "MUST match" the transitional ID too
(4.1.2.2), and QEMU's `virtio-net-pci` on the root bus of its q35
machine, the CI's, is transitional: its `disable-legacy` is off unless
the device sits behind a PCI Express port (QEMU,
`hw/virtio/virtio-pci.c`). The driver:

1. Starts as every virtio driver here does (`drivers/virtio_driver.h`):
   resets the device, turns bus mastering on, takes MSI-X table entry 0
   for its queues, and accepts VERSION_1, ACCESS_PLATFORM and
   ORDER_PLATFORM. Of the network device's own features (5.1.3) it
   accepts:
   - `VIRTIO_NET_F_MAC` (5), the address the device's network knows it
     by. A driver "SHOULD negotiate VIRTIO_NET_F_MAC if the device
     offers it" and then "MUST set the physical address of the NIC to
     mac" (5.1.4.2).
   - `VIRTIO_NET_F_MTU` (3), only if the device's `mtu` is 1,500 or less.
     The driver reads `mtu` before it accepts the feature, which a
     driver may do for a feature the device offers (3.1.1, step 4;
     2.5.2). Above 1,500 it declines the feature, departing from "A
     driver SHOULD negotiate VIRTIO_NET_F_MTU if the device offers it"
     (5.1.4.2), and its frames stay at 1,500 bytes.

   It accepts nothing else: no checksum offload (`CSUM`, `GUEST_CSUM`),
   no segmentation offload (the TSO, UFO and USO bits), no mergeable
   receive buffers (`MRG_RXBUF`), no `STATUS`, and no control queue
   (`CTRL_VQ`), and so none of the features that need one (5.1.3.1).
2. Without `VIRTIO_NET_F_MAC` offered, uses a locally administered
   unicast address drawn from `random_read`, as 5.1.4.2 advises and
   5.1.5, item 5, describes.
3. Sets up receiveq1, queue 0, and transmitq1, queue 1 (5.1.2), each of
   `NET_QUEUE_SIZE` (128) descriptors, in pages of its own pinned for
   the device; the frames lie in the section the service attaches
   ([N3](#n3-buffers-ownership-and-dma)).
4. Sets DRIVER_OK, creates the frame channel and reports with the
   service's end of it, as the input driver reports
   ([input.md K3](input.md#k3-the-report-and-the-client)). Receive
   buffers reach the device once the service attaches its section.
5. Without `STATUS` negotiated, takes the link as up: a driver that does
   not negotiate it "SHOULD assume the link is active" (5.1.4.2).

- **Why these features:** with neither mergeable buffers nor any
  receive offload, "the maximum incoming packet will be ... 1514 bytes",
  1,526 with the 12-byte header (5.1.9.3), each packet in one buffer
  (5.1.9.3.2), so every frame fits one slot of the section. A driver
  that negotiates `MTU` "MUST supply enough receive buffers to receive
  at least one receive packet of size mtu" (5.1.4.2), so the driver
  accepts it only where `mtu` fits that slot. Without `GUEST_CSUM` "the
  device MUST set flags to zero" (5.1.9.4.1), and without `CSUM` the
  driver supplies "a fully checksummed packet" (5.1.9.2.1): the stack
  computes and checks every checksum itself, so what it does depends on
  no feature of the device, and is the same on any device and under
  replay ([N16](#n16-replay)). VIRTIO 1.4 itself notes that "A truly
  minimal driver would only accept VIRTIO_NET_F_MAC" (5.1.5).
- **Cost:** no offloads, so the service checksums every byte sent and
  received in C that TinyCC compiles without optimizing; one queue pair,
  so one processor's worth of stack; frames of at most 1,500 bytes of
  payload; without `STATUS`, a link that goes down is noticed only by
  DHCP's renewal and TCP's timeouts.
- **Not taken:**
  - Checksum offload. A received buffer with `DATA_VALID` set is one the
    driver may take as checked (5.1.9.4.2), so the stack would carry a
    second path that trusts the device, and its behaviour would depend
    on the features one device offered.
  - `STATUS`, with configuration changes signalled on the queues' MSI-X
    entry, a case a driver "MUST handle" (4.1.5.4.2). Its cost is a link
    message in the frame protocol and a driver that rereads the
    configuration on every interrupt, for a first version with one
    interface and DHCP to notice a change of network.

### N3. Buffers, ownership and DMA

**Decided.** The service creates one section of `FRAME_SECTION_SIZE`
(256 KiB) and attaches it to the driver, which pins it whole for the
device to read and write ([ipc.md I8](ipc.md#i8-dma)), as the file
system service attaches a section to the block driver
(`include/block_protocol.h`). The section is `FRAME_SLOTS` (128) slots
of `FRAME_SLOT_SIZE` (2,048) bytes. A slot holds one Ethernet frame of
up to 1,514 bytes: a 14-byte header and up to 1,500 bytes of data
(RFC 894, Frame Format).

The 12-byte `struct virtio_net_hdr` (VIRTIO 1.4 5.1.9; 12 bytes when no
feature that lengthens it is negotiated, 5.1.9.3.1) never enters the
section. The driver keeps one per buffer in its own pinned pages, and
each buffer is a chain of two descriptors, the layout VIRTIO 1.4 gives
first: a transmit buffer "could be most simply placed in the descriptor
table as a 12 byte output descriptor followed by a 1514 byte output
descriptor" (2.7.4):

| Queue | First descriptor | Second descriptor |
|---|---|---|
| transmitq1 | The driver's header, all zero, device-readable: flags 0 and `gso_type` NONE, as 5.1.9.2.1 requires without `CSUM`, and `num_buffers` 0 | The frame, in its slot, device-readable |
| receiveq1 | The driver's header, device-writable | The whole slot, device-writable |

Every slot belongs to one party at a time:

| Owner | Gets the slot | May |
|---|---|---|
| The service | At the start, all of them; back from the driver in `FRAME_RECEIVED` or `FRAME_TRANSMITTED` ([N4](#n4-the-frame-protocol)) | Read and write it |
| The driver and its device | In `FRAME_RECEIVE`, to fill, or `FRAME_TRANSMIT`, to send | The device writes or reads it by DMA; the driver reads and writes nothing in it |

1. The driver reports a received frame's length from the used ring's
   `len` less the header's 12 bytes, and only for a chain it gave the
   device, since "The driver MUST NOT make assumptions about data in
   device-writable buffers beyond the first len bytes" (2.7.8.3). A
   buffer whose `len` holds no Ethernet header goes back to the device
   unreported. The driver reads nothing in the header: with none of
   `GUEST_CSUM`, `MRG_RXBUF` and the receive offloads negotiated, its
   fields hold nothing the driver acts on, and each packet comes in one
   buffer (5.1.9.3.2). QEMU, at both versions pinned, writes only the
   header's first 10 bytes and leaves `num_buffers` as the buffer held
   it, though "the device MUST set num_buffers to 1" (5.1.9.4.1; QEMU
   `hw/net/virtio-net.c`, `receive_header`), so a driver that checked
   it would drop every frame.
2. The service reads a frame only once `FRAME_RECEIVED` has returned its
   slot, only as many bytes as the message gives, and copies it once
   into its own memory before parsing it, so what is checked is what is
   used, as the input driver copies an event out of DMA memory once
   ([input.md K1](input.md#k1-the-driver)). It gives the slot back to
   the driver at once.
3. When the service closes its end, the driver resets the device before
   it releases its pins, as every virtio driver here does (VIRTIO 1.4
   3.3.1: a driver "MUST ensure a virtqueue isn't live (by device reset)
   before removing exposed buffers"). When the driver dies, its pins are
   quarantined until the first process has reset the device
   ([ipc.md I8](ipc.md#i8-dma), [I10](ipc.md#i10-driver-failure-and-restart)),
   so no slot the device can still write is used again.

- **Why:** frames move by DMA straight into and out of the service's
  pages, with no copy through the driver or the kernel: the direct I/O
  row of [ipc.md I9](ipc.md#i9-asynchronous-io-requests). Slots of one
  size make ownership a bitmap each side can check, and a slot index,
  never an address, crosses the channel. Keeping the header in the
  driver keeps every field the device acts on out of memory the service
  writes: the service gives the device frames and nothing else, and the
  protocol suits a network device whose descriptors carry no header.
- **Cost:** two descriptors per buffer, so a queue of 128 descriptors
  holds 64 buffers; 534 bytes of each slot unused; a copy of every
  received frame (rule 2); slots the driver held are lost to the service
  from the driver's death until the first process releases the
  quarantine ([N5](#n5-supervision)).
- **Not taken:** one descriptor per buffer, the header at the start of
  the slot, which 2.7.4 also allows. Its cost is a header the service
  can write while the device reads it. Buffers in the driver's own
  pages, copied to and from the service's section by the driver: one
  more copy of every frame, and a driver that touches every byte.

### N4. The frame protocol

**Decided.** `include/frame_protocol.h`, between a network driver and
its one client, the network service, on the channel the driver reports.
Every message is a `FRAME_MESSAGE`, its transaction, operation and an
entry count, followed by that many `FRAME_ENTRY`s, each a slot and a
length; all fields are little-endian.

| Operation | From | Entries | Meaning |
|---|---|---|---|
| `FRAME_ATTACH` | Service | None; one handle, the section, with `READ`, `WRITE` and `PIN` | The driver pins the section whole. Its one reply carries the transaction, a status of [syscalls.md S7](syscalls.md#s7-error-reporting), the MAC address and the MTU. A second attach fails with `ERR_BAD_STATE` |
| `FRAME_RECEIVE` | Service | Slots, length 0 | Buffers the device may fill |
| `FRAME_TRANSMIT` | Service | Slots and lengths, 14 to the MTU plus 14 | Frames to send, in order |
| `FRAME_RECEIVED` | Driver | Slots and lengths | Frames received, in the order the device used their buffers (VIRTIO 1.4 2.7.14) |
| `FRAME_TRANSMITTED` | Driver | Slots, length 0 | Frames the device has used; the slots are the service's again |

Every message after the attach has transaction 0, since none answers
another ([ipc.md I3](ipc.md#i3-synchronous-call-and-asynchronous-queues)),
and carries at most `FRAME_BATCH_MAX` (64) entries. The channel queues
`FRAME_CHANNEL_CAPACITY` (8) messages each way.

1. **Neither side ever waits on a full queue.** A write that fails with
   `ERR_SHOULD_WAIT` keeps its entries, which go out once the port
   reports the channel `WRITABLE`, and the side goes on reading
   meanwhile. Both sides write unprompted, so two sides that each
   blocked on the other's full queue would never be read again.
2. **A message that breaks the protocol ends the channel:** an unknown
   operation, a count past the message's bytes, a slot past the section
   or not the sender's to give, or a length out of range. The side that
   finds it logs it and closes its end. A driver that finds it resets
   the device, releases its pins and ends with
   `VIRTIO_NET_EXIT_CLIENT_BROKE`, which tells its supervisor the fault
   is the service's, and the restart counts against the service
   ([N5](#n5-supervision), item 3). A service that finds it waits for
   the next driver, whose restart counts against the driver.
3. Received frames the service has no room for are the device's to
   drop: a driver holding no receive buffer leaves the queue empty, and
   the device drops what it cannot place. Ethernet promises no delivery,
   and TCP recovers what is lost.

- **Why:** one message carries everything one interrupt finds, where a
  message per frame would cost each frame a page of kernel memory for
  the message, one for its bytes (`kernel/object/channel.c`) and a
  switch. Slot ownership changes hands only in a message, so each side
  knows at every moment which slots are its own.
- **Cost:** a frame can wait for the batch it goes in; the protocol
  carries no link state, multicast filter or offload, which a later
  driver needs a new operation for.
- **Not taken:** the block protocol's shape, a request answered by one
  reply (`include/block_protocol.h`). Received frames are not answers to
  anything, so the service would keep a request outstanding per receive
  buffer and the driver answer each one, a message per frame.

### N5. Supervision

**Decided.** The first process starts the service and the resolver at
startup, from the `nova-network` store path, whether or not the machine
has a network function, so that loopback and grants work without one,
and keeps both running under
[ipc.md I10](ipc.md#i10-driver-failure-and-restart)'s restart policy.
Where there is a function, it starts the driver as a driver that
reports, whose client is the service, as it starts the input driver for
the console ([input.md K3](input.md#k3-the-report-and-the-client),
[K4](input.md#k4-supervision)).

1. **Reports.** The first process hands each endpoint the driver
   reports to the service on the service's bootstrap channel, which the
   service keeps for its life, as the console keeps its channel to the
   first process: the first, and one after each restart of either. On
   each the service attaches its section and gives the driver its
   receive slots.
2. **The driver ends.** The service sees `PEER_CLOSED` on the frame
   channel and keeps every connection's state. Slots the driver held
   stay the driver's: the first process stops the function, clearing
   Bus Master Enable, resetting the device and releasing the initiator's
   quarantine, before it starts the driver again (I10), so the next
   endpoint reaches the service only once the device can no longer write
   them. On it the service attaches the same section, takes every slot
   back, and gives the receive slots out again. Frames that were in
   flight are lost, and TCP sends them again (RFC 6298 5), so a
   connection outlives a restart of the driver that ends before the
   connection's R2 threshold, which RFC 9293 3.8.3 says "SHOULD
   correspond to at least 100 seconds" (SHLD-11).
3. **The service breaks the frame protocol.** A driver that finds it
   ends with `VIRTIO_NET_EXIT_CLIENT_BROKE` ([N4](#n4-the-frame-protocol),
   rule 2). The first process starts the driver again without counting
   a restart against it, and counts one against the service. The kernel
   has no call yet that ends a process from outside
   ([accounts.md A10](accounts.md#a10-supervision)), so a service that
   breaks the protocol twice within `RESTART_PERIOD_SECONDS` cannot be
   restarted: the first process gives the network up, its function
   stopped, and logs that the service is at fault. With `task_kill`
   ([syscalls.md S8](syscalls.md#s8-the-first-calls), call 26) it would
   restart the service instead.
4. **The service ends.** Every grant and socket channel it served
   closes, and each holder sees `PEER_CLOSED`; the connections are gone,
   and their peers learn so by a reset the next time they send or by
   their own timeouts. The driver finds its client gone, resets the
   device and ends, and is started again with the service without
   counting a restart of its own, as the input driver is with the
   console (K4). Once the service is back, the first process gives the
   resolver and every system service its grant again
   ([N15](#n15-declared-authority), item 2), and login a new control
   channel, as it gives login a new console session after a restart of
   the console ([accounts.md A10](accounts.md#a10-supervision)). On it
   login makes each open session's control channel, and each running
   script's, again ([N15](#n15-declared-authority)), so a program
   started after the restart gets its grant; a program running when the
   service ended is without the network until it is started again, as a
   program whose view entry died cannot reopen it
   ([storage.md F5](storage.md#f5-per-process-views)).
5. **The resolver ends.** The service fails the names it had asked and
   asks the restarted resolver from then on.
6. **The first process never waits on the service.** Each grant or
   control channel it asks for is answered on the endpoint the request
   made ([N13](#n13-the-socket-protocol)), which the first process hands
   to the program it is for, so that program reads the answer. A service
   that stops answering stalls none of the programs the first process
   supervises, where accounts.md A10 counts waits with no deadline among
   its costs.

- **Why:** TCP's state lives in the service, so the part that holds the
  device can fail and come back with the connections none the wiser but
  for lost frames, which is what I10 promises a restarted driver's
  clients and which TCP already tolerates. A fault is charged to the
  part that made it, so a faulty service does not use up the driver's
  restarts.
- **Cost:** a restart of the service loses every connection and every
  grant in the system, and every running program's network until the
  program is started again; a service that breaks the frame protocol
  twice in a minute costs the network until the next boot, while the
  kernel cannot end it; the service's section stays pinned, its slots
  unusable, between a driver's death and the reset of its device.
- **Not taken:** the service supervising its own driver. Its cost is a
  second supervisor that must stop a PCI function, which needs the
  function's configuration space, outside the platform driver that
  holds every device's resources (I6).

## The service

### N6. Our own stack, or a port

**Decided:** our own stack, in C, shaped as this item
gives, with lwIP 2.2.1 (BSD-3-Clause) the named fallback.

The stack is written for nova-os, in C, from the RFCs this document
pins, on the design smoltcp shows: the caller passes the time in, no
heap, every buffer the caller's. lwIP 2.2.1 is the fallback, taken up if
step 2 of [N20](#n20-the-order-of-work) cannot pass its checks. The
candidates, each at the version pinned under [Network design
references](README.md#network-design-references):

| Candidate | Licence | Language | Lines | Fit |
|---|---|---|---|---|
| lwIP 2.2.1 | BSD-3-Clause (`COPYING`) | C | 23,965 of C for the IPv4 core with its DHCP client and resolver, and 12,646 of headers; 68,048 for the stack without its applications and PPP | Its `NO_SYS` mode runs "without OS-awareness (no thread, semaphores, mutexes or mboxes)", "only the callback-style raw API" (`src/include/lwip/opt.h`), on time from `sys_now`; MINIX 3 runs it as a user-mode TCP/IP service |
| picoTCP 1.7.0, picoTCP-NG 2.1 | "the GNU General Public License, version 2, or (at your option) version 3" (`COPYING`) | C | 20,861 and 23,331 for the IPv4 core | Excluded: DESIGN.md copies nothing from GPL projects |
| FreeBSD 15.1 | BSD-3-Clause and BSD-2-Clause by SPDX tag, and BSD-4-Clause in one file of `sys/netinet` (`in_cksum.c`) and five of `sys/net` | C | 219,196 in `sys/netinet`, 43,785 in `sys/netinet6`, and 109,262 in `sys/net`, which a port needs too; no DHCP client or resolver | Written for FreeBSD's kernel: `tcp_input.c` works on `struct mbuf`, takes `INP_WLOCK` and enters `NET_EPOCH` |
| smoltcp 0.14.0 | 0BSD (`LICENSE-0BSD.txt`) | Rust | 57,668 for the whole of `src/`, IPv6 and inline tests included | "a standalone, event-driven TCP/IP stack", which "does not need heap allocation *at all*" (`README.md`), polled with the time the caller gives (`poll(timestamp: Instant, ...)`): the shape of [N9](#n9-one-thread-one-port-time-and-randomness). nova-os builds no Rust compiler from its seed |
| uIP 1.0 | BSD-3-Clause | C | 7,678, and 963 for its DHCP client and resolver | One of the stacks in which AMNESIA:33 found memory-safety flaws ([N17](#n17-the-threat-model)) |

Lines are counted with `wc -l` over the source files, `.c` and `.h`
or `.rs`, at the pinned versions. lwIP's IPv4
core is `src/core`'s C files less the IPv6, `altcp`, `raw` and `stats`
ones, `src/core/ipv4` and `src/netif/ethernet.c`; its stack without
applications and PPP is `src/core`, `src/api`, `src/netif` and
`src/include` without `ppp` and `apps`. picoTCP's IPv4 core is `stack/`,
`include/` and its Ethernet, ARP, IPv4, ICMP, fragment, UDP, TCP, DHCP
client and DNS client modules. FreeBSD's and smoltcp's are every
source file of the directories named, and uIP's those of `uip/`, with
`apps/dhcpc` and `apps/resolv` apart.

Weighed on what the stack must be:

- **Auditability.** nova-os's tree holds no third-party source: every
  file is written for it, from public documents, as DESIGN.md's licence
  decision describes, down to Argon2 and BLAKE2b written from their
  RFCs ([accounts.md A4](accounts.md#a4-hashing-passwords)). A stack of
  its own is written to the same rules, one configuration and named
  bounds. lwIP serves every build from one tree through compile-time
  options in `opt.h`, and carries its own conventions; FreeBSD's is a
  kernel's, and larger than the rest of nova-os.
- **Correctness.** lwIP has the field history: its copyright runs from
  2001, and it is not among the stacks AMNESIA:33 names, whose CERT/CC
  note carries Espressif's statement that its SDKs "use the TCP/IP
  library lwIP, therefore are not affected" (VU#815128). A new stack
  starts with none, and AMNESIA:33's "33 memory related
  vulnerabilities" in small embedded stacks show what young parsers of
  network input get wrong. A stack of nova-os's own earns
  its confidence from the RFCs' requirements met one by one
  ([N7](#n7-what-v1-implements)), from CI against a second,
  independent TCP, libslirp's, derived from 4.4BSD's
  ([N21](#n21-the-ci-test-plan)), and from host checks of every parser
  on malformed input.
- **Size.** v1's scope is about lwIP's IPv4 core, so neither is small;
  a stack of one configuration carries none of the code for the others.
- **Fit with handles and replay.** lwIP's `NO_SYS` mode fits
  [N9](#n9-one-thread-one-port-time-and-randomness): one context,
  time from `sys_now`, randomness from `LWIP_RAND`. But its DHCP client
  and resolver are in its core, where [N11](#n11-names-and-the-resolver)
  puts the resolver in a process of its own; its memory is pools shared
  by every connection, where [N18](#n18-limits-on-clients) gives each
  client its own share; and its callbacks are wrapped into the channel
  protocol either way. MINIX 3 did that wrapping, and the commit that
  added its service says it uses lwIP "for maintenance reasons".
  FreeBSD's stack needs its kernel's mbufs, locks and epochs emulated in
  one process.
- **Ownership.** Fuchsia's Netstack3 is "a networking stack being
  developed from scratch in Rust" (Contributing to Netstack3), to
  replace a netstack "owned by the gVisor team who have different
  priorities, use cases and design goals than Fuchsia" (the Netstack3
  roadmap). A port of lwIP is a fork nova-os keeps in step
  with upstream or owns outright.

- **Why our own:** the stack is the most exposed parser in the system;
  written to the house's rules, from the documents pinned here, its
  every line is one the project has read and chosen. Its design
  can follow N9 and N18 from the start rather than be bent to them.
- **Cost:** time before the first connection, and the bugs lwIP met and
  fixed over two decades met again; the checks of
  [N21](#n21-the-ci-test-plan) are what stand between them and users.
- **Not taken:** a port of lwIP. Its cost is the first third-party
  source in the tree, a fork to keep, its resolver and memory model to
  rework for N11 and N18, and a build with TinyCC that nobody has
  tried. FreeBSD's stack: a kernel's stack, larger than the rest of
  nova-os, to run in one process. smoltcp: a Rust compiler that the
  seed does not build. picoTCP: GPL.

### N7. What v1 implements

**Decided.**

| Layer | Implemented from | v1 does | v1 leaves out |
|---|---|---|---|
| Ethernet | RFC 894, Frame Format | Type 0800h for IPv4 and 0806h for ARP; frames for its own address or broadcast; padding to the 46-byte minimum; payloads of up to the MTU | VLAN tags, multicast groups, other types (dropped) |
| ARP | RFC 826; RFC 1122 2.3.2 | Requests and replies, with RFC 826's update of an entry already held (Packet Reception); entries that time out, `ARP_TIMEOUT_SECONDS` (60), "on the order of a minute" (2.3.2.1); at most one request a second per address, the rate 2.3.2.1 recommends; the latest packet for an unresolved address kept and sent once it resolves (2.3.2.2); at most `ARP_ENTRIES` (64) entries, the least recently used replaced | Proxy ARP |
| IPv4 | RFC 791 3.1, 3.2; RFC 1122 3.2.1, 3.3 | Header checksums made and checked; one address and prefix, one router, chosen as RFC 1122 3.3.1 routes outbound datagrams; reassembly, which a host "MUST implement" (3.3.2), up to an EMTU_R of 1,500 bytes, with a timer of 15 seconds, RFC 791's "recommendation for the initial timer setting" (3.2); `127.0.0.0/8` on the loopback interface only, never on the wire (RFC 1122 3.2.1.3) | Fragmenting outbound datagrams, which RFC 1122 3.3.3 makes optional; IP options in either direction ([below](#departures)); forwarding |
| ICMP | RFC 792; RFC 1122 3.2.2 | The Echo server every host "MUST implement" (3.2.2.6) and Echo Requests for echo sockets (Echo or Echo Reply Message); Destination Unreachable, port unreachable, for UDP to a port nobody listens on (RFC 1122 4.1.3.1); errors passed to the connection that caused them, checked as RFC 5927 4.1 checks them | Timestamp, Information, Address Mask and Redirect handling beyond dropping them |
| UDP | RFC 768; RFC 1122 4.1 | Datagrams of a connected socket; checksums always made, and a datagram with "a checksum that is non-zero and invalid" silently discarded (4.1.3.4) | Datagrams larger than the MTU allows, since v1 does not fragment; listening sockets |
| TCP | RFC 9293; RFC 6298; RFC 5681; RFC 6528; RFC 5961; RFC 6056 | Every MUST of RFC 9293 but source routing (MUST-51 to MUST-53, [below](#departures)), among them: the MSS option (3.7.1, MUST-14 to MUST-16); the retransmission timer of RFC 6298 with Karn's algorithm (MUST-18), its initial RTO of 1 second (2.1) and minimum of 1 second (2.4); slow start, congestion avoidance and fast retransmit and recovery (RFC 5681 3.1, 3.2; MUST-19); initial sequence numbers as RFC 6528 3 makes them (MUST-8, MUST-9, [N9](#n9-one-thread-one-port-time-and-randomness)); TIME-WAIT for twice the MSL of 2 minutes (3.4.2; 3.6, MUST-13); zero-window probing (MUST-36); silly window avoidance (3.8.6.2); delayed acknowledgments under 0.5 seconds (MUST-40); Nagle's algorithm, with a way to turn it off (SHLD-7, MUST-17); urgent data received, with a notice when it begins or advances and a count of what remains (MUST-30 to MUST-33); ICMP soft errors reported and never aborting (3.9.1.8, MUST-47; 3.9.2.2, MUST-56, SHLD-25) and hard errors aborting (SHLD-26); R2 settable per connection, and for a SYN at least 3 minutes (3.8.3, MUST-21, MUST-23); a local address an open may name (3.9.1.1, MUST-43); opens to a broadcast or multicast address refused (MUST-46); a Differentiated Services field each connection may set (3.9.1.9, MUST-48); a TTL of `IP_TTL` (64), which the system recipe may change (MUST-49); RFC 5961's challenge ACKs and acceptable-ACK check (3, 4, 5) with its ACK throttling (7); ephemeral ports by RFC 6056's Algorithm 3 (3.3.3) over 1024 to 65535 (3.2) | Window scaling, timestamps and selective acknowledgment; ECN (SHLD-8); path MTU discovery, which RFC 9293 3.7.2 strongly recommends; keep-alives, which "MUST default to off" (MUST-25) where they exist |
| DHCP | RFC 2131; RFC 2132 | The client ([N10](#n10-dhcp-and-the-interface)) | DHCPINFORM; static configuration |
| DNS | RFC 1034; RFC 1035; RFC 5452; RFC 7766 | The stub resolver ([N11](#n11-names-and-the-resolver)), A records | AAAA and every other type; a cache |

The service also serves a loopback interface, `127.0.0.1/8`, on which a
program's connection to another program's listener never leaves the
service.

#### Departures

v1 sends no IP options and skips received ones by the header's length.
RFC 1122 3.2.1.8 requires that "All IP options (except NOP or
END-OF-LIST) received in datagrams MUST be passed to the transport
layer", where each layer "MUST each interpret those IP options that they
understand and silently ignore the others"; no layer of v1 understands
any, so ignoring them meets that. The same section also requires "a
means for the transport layer to specify IP options to be included in
transmitted IP datagrams", which v1 lacks: v1 departs from that MUST,
and with it from RFC 9293's source routing, which an application "MUST
be able to specify ... when it actively opens a TCP connection"
(3.9.2.1, MUST-51 to MUST-53). No program v1 is for needs either.

v1 also sends datagrams and segments of up to its MTU to every
destination, departing from RFC 1122 3.3.3's advice that "the IP layer
SHOULD use EMTU_S <= 576 whenever the destination address is not on a
connected network". RFC 9293 instead recommends deriving the MSS "from
the MTUs of connected interfaces" (3.7.2), as v1 does, and leaves the
rest to path MTU discovery, which v1 lacks.

- **Why this set:** it is what DHCP, ping, the CI's echo tests, a name
  lookup and later SSH need, and each requirement it meets is one RFC
  1122 or RFC 9293 makes of every host. TCP's left-out extensions are
  performance features: with no window scaling a window is at most
  65,535 bytes (RFC 9293 3.1), which the 32 KiB buffers of
  [N18](#n18-limits-on-clients) do not reach anyway.
- **Cost:** throughput bounded by one window per round trip, 32 KiB;
  no path MTU discovery, so a path whose MTU is below the interface's
  relies on routers fragmenting, as IPv4 allows when DF is clear; no
  IP options for any program.
- **Not taken:** a larger first version, with window scaling,
  timestamps and SACK. Their cost is more TCP to get right before the
  first connection is proved, for throughput no first use needs.

### N8. IPv6

**Decided:** IPv4 first, and IPv6 next, with every
address in the protocols a family and 16 bytes from the start.

v1 is IPv4 only; IPv6 comes in the next version. Every address in the
grant and socket protocols ([N12](#n12-grants),
[N13](#n13-the-socket-protocol)) carries a family and 16 bytes from the
start, so adding IPv6 changes the layout of no client protocol. The
rule strings gain IPv6 addresses, and `*` its meaning for IPv6's
loopback and link-local addresses, which the IPv6 version specifies.

What an IPv6 host takes, in RFC 8504's list of node requirements: IPv6
itself (RFC 8200; 5.1); Neighbor Discovery, which "MUST be supported"
(RFC 4861; 5.4); ICMPv6, which "MUST be supported" (RFC 4443; 5.8);
stateless address autoconfiguration, which "Hosts MUST support" (RFC
4862; 6.3), with addresses that do not embed the MAC address
recommended over RFC 4862's own; MLDv2 for every node "that need[s] to
join multicast groups" (5.11), as Neighbor Discovery's solicited-node
groups need (RFC 4861 7.2.1); the router advertisement's DNS option,
which implementations "MUST include support for" (8.3); and AAAA
records (7). A host with both families also needs to choose between
them when it connects by name, which Happy Eyeballs (RFC 8305)
specifies.

- **Why later:** that list is about as large as v1's IPv4 core, and
  none of the first milestones needs it ([N20](#n20-the-order-of-work)):
  the networks v1 is tested on give IPv4 by DHCP. Proving TCP once, over
  IPv4, before its addresses double keeps the first failures in one
  place.
- **Cost:** a machine on an IPv6-only network has no network until the
  next version.
- **Not taken:** both families in v1. Its cost is the list above before
  the first ping.

### N9. One thread, one port, time and randomness

**Decided.** The service runs on one thread that waits on one port.
Every channel it serves has a one-shot watch on that port, re-armed once
the channel is drained ([ipc.md I4](ipc.md#i4-ports)); `port_wait`'s
deadline is the earliest of its timers (retransmission, delayed ACK,
TIME-WAIT, ARP, reassembly, DHCP), since the core has no timer object
and "a server multiplexes its own timeouts"
([objects.md O2](objects.md#o2-object-types)).

1. The time comes only from `clock_monotonic`. RFC 6528's clock M, "the
   4 microsecond timer", is the monotonic clock in nanoseconds divided
   by 4,000.
2. Randomness comes only from `random_read`: at start, the keys of the
   initial sequence numbers and of the ephemeral ports; then DHCP's
   first `xid` and the random parts of its timers, and each datagram
   socket's port ([N11](#n11-names-and-the-resolver)). While the
   kernel's generator is unseeded, `random_read` fails with
   `ERR_BAD_STATE` ([syscalls.md S8](syscalls.md#s8-the-first-calls)),
   and the service opens no connection and sends no DHCP message until
   it succeeds, since numbers an attacker could predict defeat both.
3. RFC 6528's F is keyed BLAKE2b (RFC 7693 3.3) over the four values
   that name the connection, with a 32-byte key; the ephemeral ports'
   hash of RFC 6056 Algorithm 3 is the same function under its own key.
   BLAKE2b is the accounts service's (`accounts/blake2b.c`), moved to
   `lib/` for both.
4. The service never waits on any one channel: a client that reads
   slowly fills its own queue, and the service answers it when its
   watch reports it `WRITABLE`.

- **Why:** with one thread, and inputs only from messages, frames,
  `clock_monotonic` and `random_read`, the service's behaviour is a
  function of its inputs in order; replay.md records all of them but
  the frames ([N16](#n16-replay), item 6), and its thread interleaving
  does not arise. RFC 6528's F
  "MUST NOT be computable from the outside" (RFC 9293 3.4.1, MUST-9),
  which a keyed hash meets; the RFC suggests MD5, but notes that
  "implementations should consider the trade-offs involved in using
  functions with stronger security properties", and nova-os already has
  BLAKE2b, checked at start against RFC 7693's test vectors.
- **Cost:** one processor's worth of protocol processing for the whole
  system; a timer queue in the service.
- **Not taken:** a thread per connection or per client. Its cost is
  locks around state every connection shares (the ARP cache, the port
  table, the frame slots), and thread interleaving that a recording of
  the service would have to capture.

### N10. DHCP and the interface

**Decided.**

The service configures its one interface by DHCP, as RFC
2131's client (4.4):

1. From INIT it broadcasts DHCPDISCOVER, takes the first DHCPOFFER, sends
   DHCPREQUEST and is BOUND on DHCPACK (3.1); it renews from T1 and
   rebinds from T2, which default to half and seven eighths of the lease
   (4.4.5), each with a random fuzz.
2. It retransmits after 4 seconds, then 8, doubling to at most 64, each
   "randomized by the value of a uniform random number chosen from the
   range -1 to +1" (4.1). Its first `xid` is random and the rest follow
   it (4.1).
3. Before using an offered address it probes it with ARP, and sends
   DHCPDECLINE if another host answers (4.4.1); once it uses it, it
   broadcasts an ARP reply announcing it (4.4.1).
4. It asks for the subnet mask (RFC 2132 3.3), the router (3.5), the
   name servers (3.8), the lease time (9.2) and T1 and T2 (9.11, 9.12),
   in the parameter request list (9.8), and sends no Host Name option
   (3.14), so its lease does not name the machine.
5. When the lease expires unrenewed it stops using the address, as RFC
   2131 4.4.5 requires, and every connection bound to it is reset.
6. It keeps the router and up to `NAME_SERVERS_MAX` (3) name servers,
   ignoring a loopback, unspecified, broadcast or multicast address
   given as one, and logs every change of configuration on one line:
   the address and prefix, the router, the name servers, the lease and
   its server.

The control channel's status request reports the same
([N13](#n13-the-socket-protocol)).

- **Why in the service:** DHCP's whole output is the stack's
  configuration, and a client that can set the router can put any host
  on the path of every connection, which is what a rogue DHCP server can
  already do. A separate DHCP process would hold almost the service's
  power over traffic and add a protocol to give it.
- **Cost:** the service parses DHCP replies from any host on the link.
  No static address: a network without a DHCP server gives the machine
  no address in v1.
- **Not taken:** the DHCP client as its own process, as the resolver is
  ([N11](#n11-names-and-the-resolver)). Its cost is a configuration
  protocol into the service, for a separation that moves no real power
  out of the client.

### N11. Names and the resolver

**Decided.**

Names are resolved for programs, never by them. A program
connects to a name ([N13](#n13-the-socket-protocol)); the service checks
the name against the program's grant ([N12](#n12-grants)) and sends it
to the resolver, a process of its own, which answers with addresses.

1. The service answers `localhost` and names under `.localhost` itself,
   with 127.0.0.1, never asking: "Name resolution APIs SHOULD NOT send
   queries for localhost names to their configured caching DNS
   server(s)" (RFC 6761 6.3, item 3).
2. The resolver is a stub resolver (RFC 1034 5.3.1): it asks the name
   servers DHCP gave, in order, for A records, with recursion desired,
   over UDP first and over TCP when an answer is truncated, which "Stub
   resolver implementations ... MUST support" (RFC 7766 5), with TCP's
   two-byte length (RFC 1035 4.2.2).
3. Each query has an unpredictable ID, from "the full range
   available", and an unpredictable source port (RFC 5452 9.2): the
   service draws a datagram socket's port at random from 1024 to
   65535, RFC 6056's Algorithm 1 (3.3.1), where a stream's comes from
   Algorithm 3. An answer counts only if its source address, its
   destination address and port, ID, name, class and type match the
   query's (RFC 5452 9.1).
4. The parser accepts names of at most 255 bytes and labels of at most
   63 (RFC 1035 2.3.4), compares them without regard to case (2.3.3),
   follows a compression pointer only to an earlier offset, as the
   format's "pointer to a prior occurance" (4.1.4) allows, so a message
   cannot make it loop, and follows at most `NAME_ALIASES_MAX` (8)
   CNAMEs.
5. The resolver keeps no cache; each name is asked when a connection
   needs it.

- **Why names are resolved for programs:** a grant names hosts as people
  do, by name, and only the service can check a name it resolves
  itself. A program that resolved names itself would need a grant to
  the name servers, and a program that can ask any question can send
  data to any domain's name servers in the names it asks about; here a
  program can only cause a lookup of a name its grant already allows.
- **Why a process of its own:** the resolver parses answers from servers
  nobody here controls, the kind of parser a forged answer attacks. A
  compromised resolver can answer with false addresses, which forged DNS
  answers already let an attacker do, and nothing more: it holds no
  program's sockets, never learns which program asked for a name, and
  its own grant reaches only the name servers' port 53.
- **Cost:** a process, a protocol and a message round trip per name;
  with no cache, a lookup per connection.
- **Not taken:** the resolver in the service. Its cost is that a bug in
  parsing an answer from any server on the Internet would hand over
  every program's connections. A resolver library in each program, as
  Unix does: every program would need its own grant to the name
  servers, and could signal through its queries (above).

## Sockets and authority

### N12. Grants

**Decided.**

No call opens a socket from nothing. The network is reached only
through a grant: a channel to the network service that the service
serves with a set of rules, which say whom its holder may reach and on
which ports it may listen. A program holds a grant only if its creator
sends it one, as with every other handle
([objects.md O6](objects.md#o6-no-ambient-authority)).

A rule is written as an action, a protocol, a host and, but for
`icmp`, ports:

```
connect tcp cache.nixos.org 443
connect tcp *.example.org 443
connect tcp 10.0.2.0/24 1-65535
connect udp * 123
connect icmp *
connect tcp localhost 8080
listen tcp * 22
listen tcp localhost 8080
connect udp nameservers 53
```

| Part | Values |
|---|---|
| Action | `connect`, for connections and datagrams the holder opens and for echo requests (`icmp`); `listen`, for connections others open to it, TCP only in v1 |
| Protocol | `tcp`, `udp`, `icmp` |
| Host, for `connect` | A name, which covers that name only; `*.` and a name, which covers every name below that name and not the name itself; an IPv4 address with a prefix length, `/32` if none is given; `localhost`, the address 127.0.0.1 the service gives that name ([N11](#n11-names-and-the-resolver)); `nameservers`, the name servers the service's configuration names at the time; `*`, every name and every address but loopback |
| Host, for `listen` | `*`, every local address but loopback; or a local address with a prefix length, `/32` if none is given, such as `localhost`, 127.0.0.1, for loopback |
| Ports | A port, a range `first-last`, or `*` for 1 to 65535 |

A name is labels of ASCII letters, digits and hyphens, each starting
and ending with a letter or digit (RFC 1035 2.3.1, whose first
character RFC 1123 2.1 relaxes "to allow either a letter or a digit"),
compared without regard to case (RFC 1035 2.3.3); a name outside ASCII
is written in its encoded form. A host that reads as a dotted-decimal
IPv4 address is an address, never a name, as RFC 1123 2.1 has a host
"check the string syntactically for a dotted-decimal number before
looking it up".

**Coverage.** A rule covers another of the same action and protocol
whose ports lie within its own and whose host lies within its own:

- for `connect`, a name lies within itself and within a `*.` rule it
  lies below; a `*.` rule within any `*.` rule above it; a prefix within
  any prefix that holds it; `nameservers` within itself; and every
  name, `*.` rule, `nameservers` and prefix that holds no loopback
  address within `*`. A loopback prefix lies only within a prefix that
  holds it.
- for `listen`, a local address or a prefix lies within any prefix
  that holds it, and, if it holds no loopback address, within `*`.

In a rule, `localhost` is the prefix 127.0.0.1/32. Since `*` never
covers loopback, a program reaches a local program, and serves one,
only through a rule that names loopback: `connect tcp localhost 8080`,
`listen tcp localhost 8080`.

The root's rules ([N15](#n15-declared-authority)) are `*` and
`127.0.0.0/8` for each action and protocol, with every port, which
together cover every name and every address; a prefix that holds both
loopback and other addresses lies within neither, so a declaration
writes it as `*` and a loopback prefix. A grant covers a rule if one of
its rules does, and a set of rules if it covers each.

**Checks.**

1. A connect by name needs a rule that covers the name; the service
   then resolves it ([N11](#n11-names-and-the-resolver)). A connect by
   address needs a rule that covers the address. `localhost` and the
   names below it are not checked as names: the service resolves them
   to 127.0.0.1 itself and checks that address.
2. No connect reaches an unspecified, broadcast or multicast address,
   whatever the rules: RFC 9293 has TCP "reject as an error a local
   OPEN call for an invalid remote IP address (e.g., a broadcast or
   multicast address)" (3.9.1.1, MUST-46), and v1 does the same for
   datagrams and echoes. No name but `localhost` and the names below it
   reaches loopback: an answer that holds a loopback address is refused
   with `ERR_ACCESS_DENIED`, whatever rule covered the name.
3. A listen needs a rule that covers the local address and port; a
   listen on a port a listener already holds at an address that
   overlaps its own, by any client, fails with `ERR_ALREADY_EXISTS`.
   `*` and a loopback address do not overlap, so one program can listen
   on both.
   There are no privileged ports: the grant is the only authority a
   listen needs.
4. A refused request sends nothing to the network.

**The log** is a setting of the network service, `network.log`,
declared in its interface description and exposed through the generic
bridge like any service's setting
([shell.md L24](shell.md#l24-settings-and-service-commands)). Its
default, `refusals`, logs each refused request and each listener
opened; `connections` logs, beside them, every connection opened and
accepted. Each line names the client's label, the host and the port,
never data. The setting is the system's, so only an administrator's
settings handle changes it.

- **Why:** principle 7 bounds a program by what its derivation declares,
  and a list of rules is that declaration for the network, checkable
  before the program runs ([N15](#n15-declared-authority)). Names are
  what a derivation can sensibly declare, since addresses change; so the
  service resolves them, and checks the name the program asked for. The
  loopback rule keeps a name rule from being turned by a hostile name
  server on a service listening only on loopback, whose authors meant it
  for local programs granted it by address; the root names loopback
  apart for the same reason, since `*` leaves it out.
- **Why `*` leaves loopback out:** a service on loopback is written for
  the machine's own programs, and a rule meant as "the network" must not
  reach it. Modern sandboxes draw the same line. Chrome's Private
  Network Access draft gives loopback, 127.0.0.0/8, an address space of
  its own, which "contains the local host only", and restricts a
  request to an address less public than its origin's (Private Network
  Access, 2.1, 2.2). Local Network Access, which "builds on top of
  Chrome's previously paused" Private Network Access work, keeps
  loopback apart under that name and counts the null address 0.0.0.0
  in it, "as some systems can still route them back to the local host
  machine" (Local Network Access, 1, 2.1); check 2 here refuses that
  address outright. So local access is written in the rule, where a
  reader of the declaration sees it, for a client and a server alike.
- **Why the log is a setting:** a refusal is a program asking for more
  than it declared, which an audit wants, and a listener is the machine
  open to others; a line per connection puts a user's every destination
  in the system's log, which an administrator may need and should
  choose.
- **Cost:** a program that must connect to an address it learns at run
  time, such as a peer in a peer-to-peer protocol, needs a broad address
  rule; a name rule trusts DNS for where the name leads, which only TLS
  in the program checks ([N19](#n19-tls)); `*` includes the local
  network's addresses, its router's among them, with no rule meaning
  "the Internet only"; a program that serves both the network and the
  machine's own programs declares two listen rules and opens two
  listeners; with `connections` set, the log holds every destination of
  every user.
- **Not taken:** rules by address only, as a packet filter writes them.
  Their cost is a declaration that is stale as soon as a host moves,
  and no way to say "this program talks to that service". An
  "open any socket" grant for every program, as Unix and Windows give:
  ambient authority, which principle 7 rules out. `*` as POSIX's
  wildcard: `INADDR_ANY`, the "IPv4 wildcard address" a socket gives
  `bind()` as its local address (POSIX, `<netinet/in.h>`), names no
  address and so leaves none out. Its cost is a rule meant as the
  network that takes loopback in too, so `listen tcp * 22` would serve
  the machine's own programs and a connect rule of `*` reach what listens
  only for them. `*` as public addresses only, with a host class for the
  local network, as Private Network Access also sets the private
  network apart. Its cost is pinning the private and link-local ranges,
  which nothing in v1 needs. Logging nothing, since the grant states
  what a program could reach and a recording what it did; its cost is a
  refusal no one sees. Every connection always; its cost is every
  user's every destination in every machine's log, whether or not its
  administrator wants it.

### N13. The socket protocol

**Decided.**

`include/network_protocol.h`. Every kind of network object is a channel
to the service. Every message starts with an `IO_REQUEST` and every
reply with an `IO_REPLY` (`include/directory_protocol.h`), as
[ipc.md I9](ipc.md#i9-asynchronous-io-requests) shapes I/O; requests
that make a channel take an endpoint the client created, as
[objects.md O8](objects.md#o8-the-namespace)'s open does, and the reply
goes on the endpoint. Addresses are a `NET_ADDRESS`: a family, a port
and 16 bytes, of which IPv4 uses 4.

**On a control channel** (the first process's, login's, and one per
session, [N15](#n15-declared-authority)):

| Request | Effect | Needs |
|---|---|---|
| `NET_GRANT(endpoint, label, rules)` | Serves the endpoint as a grant with these rules, starting a new client ([N18](#n18-limits-on-clients)) from this channel's clients; the label names the client in the service's log | `NET_RIGHT_CLIENTS`; the channel covers the rules and has a client left |
| `NET_CONTROL(endpoint, rights, rules, clients)` | Serves the endpoint as a control channel with these rights and rules, and `clients` of this channel's clients, at least 1, reserved for it until it and every client it started have ended | Each right it gives; the channel covers the rules and has the clients left |
| `NET_STATUS()` | The interface's configuration ([N10](#n10-dhcp-and-the-interface)) and counts of clients and connections | `NET_RIGHT_STATUS` |

**On a grant:**

| Request | Effect |
|---|---|
| `NET_DERIVE(endpoint, rules)` | Serves the endpoint as a grant with these rules, which this grant must cover, in this grant's client |
| `NET_RULES()` | This grant's rules |
| `NET_CONNECT(endpoint, protocol, host, port, local, rights, flags, r2, ds)` | For `tcp`, the reply comes once the handshake completes, with the local and remote addresses, and the endpoint is then a stream; or it fails with `ERR_CONNECTION_REFUSED`, `ERR_UNREACHABLE`, `ERR_TIMED_OUT`, `ERR_NOT_FOUND` for a name with no address, or `ERR_ACCESS_DENIED` for a host or port no rule covers ([N12](#n12-grants)). For `udp`, the endpoint is a datagram socket bound to that peer. `local` is the local address to send from, or none for the service to choose (MUST-43); `rights` are `IO_RIGHT_READ`, to receive, and `IO_RIGHT_WRITE`, to send; `flags` hold `NET_NO_DELAY`, which turns off Nagle's algorithm (MUST-17); `r2` is RFC 9293 3.8.3's R2 in seconds, 0 for the defaults, 100 for data and 180 for a SYN (MUST-21, MUST-23); `ds` is the Differentiated Services field its segments carry (MUST-48) |
| `NET_LISTEN(endpoint, protocol, host, port, backlog)` | Serves the endpoint as a listener |
| `NET_ECHO(endpoint, host)` | Serves the endpoint as an echo socket to that host |

**On a stream** (a TCP connection):

| Request | Effect | RFC 9293 3.9.1 |
|---|---|---|
| `IO_READ(length)` | Replies once data has arrived, with up to `length` bytes, at most `IO_TRANSFER_MAX` (32 KiB); 0 bytes once the peer has finished sending. The reply's flags mark bytes that end urgent data, and its information counts the urgent bytes still unread (MUST-33) | RECEIVE |
| `IO_WRITE(bytes)` | Replies once every byte is in the send buffer; a flag marks them urgent | SEND |
| `NET_FINISH()` | Sends FIN after the queued bytes; reading goes on | CLOSE |
| `NET_ABORT()` | Sends RST and ends the connection | ABORT |
| `IO_GET_ATTRIBUTES()` | The state, the local and remote addresses, the bytes queued each way, and the last soft error, an ICMP error that did not end the connection (MUST-47) | STATUS |
| Closing the channel | As `NET_FINISH`, and the service finishes the close on its own; if received data was unread, or more arrives, it sends RST "to show that data was lost" (RFC 1122 4.2.2.13) | CLOSE |

When urgent data begins, or its pointer advances, the service writes
one message unasked, transaction 0, `NET_URGENT`, as RFC 9293 asks a
TCP to "inform the application layer asynchronously" (3.8.5, MUST-32).
A connection reset by the peer fails the next request with
`ERR_CONNECTION_RESET`; one closed normally reads 0 bytes, so the
program learns which, as RFC 1122 4.2.2.13 requires. Cancellation is
I9's.

**On a listener:** `NET_ACCEPT(endpoint, rights, flags, r2, ds)`
replies on the endpoint once a connection has completed its handshake,
with the remote address, and the endpoint is then a stream.

**On a datagram socket:** `IO_WRITE` sends one datagram of at most the
MTU less the IPv4 and UDP headers, 1,472 bytes at an MTU of 1,500;
`IO_READ` replies with one datagram, flagged if it was cut to
`length`.

**On an echo socket:** `NET_ECHO_SEND(sequence, bytes)` sends one Echo
Request and replies when the matching Echo Reply comes, with the round
trip in nanoseconds; the client's deadline governs, by I9's cancel.

The statuses are [syscalls.md S7](syscalls.md#s7-error-reporting)'s,
with three more that only completions carry, as codes 16 to 19 are:
`ERR_CONNECTION_REFUSED` (20), `ERR_CONNECTION_RESET` (21) and
`ERR_UNREACHABLE` (22).

- **Why channels:** a socket is then an object reached by a handle,
  like a file (principle 8): transferable, waitable on a port with every
  other channel, recorded by the kernel like any message, and closed
  with its holder. A stream answers `IO_READ` and `IO_WRITE` as a file
  does, so code that reads a file reads a connection. RFC 9293's user
  interface (3.9.1) maps onto it, but for FLUSH, which "MAY be
  implemented" (3.9.1.7, MAY-14) and has no request.
- **Why data inline:** every byte a program reads arrives in a message,
  which the kernel records ([N16](#n16-replay)); a section shared with
  the service would be one replay.md forbids a recorded process.
- **Cost:** each byte is copied into the kernel and out again, twice
  per hop; a read is a request and a reply, a round trip the program
  waits for unless it keeps one outstanding; each queued message holds
  kernel pages, a page for the message and one per 4 KiB of its bytes
  (`kernel/object/channel.c`).
- **Not taken:**
  - A ring of buffers in a section each socket shares with the service,
    which moves data with no copy through the kernel, for all programs
    or beside inline data for the unrecorded. Its cost is that a
    recorded program could not use it, since it could not share a
    section the service writes ([N16](#n16-replay)), and a second data
    path before any program is measured to need more than inline
    gives.
  - The service writing data to the client as it arrives, unasked,
    which saves the read request. Its cost is a stream that no longer
    reads like a file, and data held in the kernel's queue rather than
    the service's window, where TCP's flow control cannot see it.
  - Berkeley sockets as the native interface. A POSIX subsystem can
    provide them over this protocol (DESIGN.md, principle 2); as the
    base they would bring a call that opens a socket from nothing.

### N14. Rights on a network handle

**Decided.** Every network object is a channel, so its handle carries
the kernel's channel rights: `READ`, `WRITE`, `WAIT` and `TRANSFER`,
and never `DUPLICATE` ([ipc.md I1](ipc.md#i1-channels)), so each has
one holder. A client needs `READ` and `WRITE` on any of them, since
every request is a message written and every answer one read.

The service records, for each channel it serves, the rights of the
protocol, set when the channel is made and never widened, as the
directory protocol's rights narrow at each open
([storage.md F1](storage.md#f1-the-directory-protocol)):

| Channel | Rights |
|---|---|
| Control | `NET_RIGHT_CLIENTS`: grants that start clients; `NET_RIGHT_STATUS`: the interface's status; its rules, which bound every grant it gives; and its clients, reserved from the channel it came from |
| Grant | Its rules |
| Stream, datagram socket | `IO_RIGHT_READ`: receive; `IO_RIGHT_WRITE`: send |
| Listener, echo socket | None beyond the requests the kind allows |

- **Why the protocol's rights:** the kernel's `READ` and `WRITE` cannot
  carry "may receive but not send", because receiving is itself a
  request written to the channel. A program can open a connection that
  only receives and hand it to a less trusted process, a decompressor
  for a download, which then cannot send a byte.
- **Cost:** the kernel does not enforce these rights; the service does,
  as file servers enforce theirs (O8).
- **Not taken:** a channel per direction. Its cost is two handles per
  connection, two watches, and closing one direction by closing a
  handle, which TCP's half close does not match.

### N15. Declared authority

**Decided.**

Every grant comes from a control channel, and every control channel
from the one above it, so each level can only narrow the one above.

1. **The root.** The first process holds the service's control channel,
   with every right, the root's rules ([N12](#n12-grants)), which cover
   every name and every address, and all `NET_CLIENTS` clients. It is
   the only control channel the service starts with.
2. **System services.** The first process gives each service that needs
   the network a grant through `NET_GRANT` on the root, with the rules
   the system recipe declares for it, from `NET_SYSTEM_CLIENTS` (4)
   clients it keeps for them: the resolver's rules are `connect udp
   nameservers 53` and `connect tcp nameservers 53`; a substituter's, its
   substituters' hosts and ports
   ([storage.md F12](storage.md#f12-builds-and-substitution)); the store
   service's, the rules it narrows for each fixed-output build, which
   are the only builds that get a network handle (F12).
3. **Login.** The first process gives login a control channel through
   `NET_CONTROL`, with `NET_RIGHT_CLIENTS`, the rules the system recipe
   gives any account's sessions, the outbound rules of item 4 among
   them, and the rest of the clients, 12; and a
   new one after each start of login or of the service
   ([N5](#n5-supervision)). Login holds no grant of its own.
4. **Sessions.** For each session it opens, login makes a control
   channel of the session's through `NET_CONTROL`, with the rules the
   recipe gives that account, or for an account it names none for,
   outbound only: `connect tcp * *`, `connect udp * *` and
   `connect icmp *`. The channel has `NET_SESSION_CLIENTS` (4) clients,
   and login keeps it. The shell gets no grant, nor does the HUD's or a
   terminal's: the session's channel serves only the grants of the
   programs and scripts login starts (items 5 and 6;
   [accounts.md A7](accounts.md#a7-the-session)). When the session ends,
   login closes the session's control channel, and its clients return
   as the programs that hold them end.
5. **Programs.** A program's derivation declares its rules in a
   `network` attribute, a list of rule strings as [N12](#n12-grants)
   writes them. Login, the launcher, reads the declaration from the
   program's deriver, as
   [shell.md L20](shell.md#l20-the-recipe-and-the-command-line) has the
   launcher read declarations, and gives the program a grant through
   `NET_GRANT` on its session's control channel with exactly the
   declared rules.
   The service refuses rules the session's channel does not cover, with
   `ERR_ACCESS_DENIED` naming the rule, and a session with no client
   left, with `ERR_NO_RESOURCES`; either fails the launch, naming the
   cause, as [storage.md F5](storage.md#f5-per-process-views), rule 3,
   fails a launch. A program whose derivation declares nothing gets no
   grant. The shell asks login to run a program and takes no part in
   its grant.
6. **Scripts.** A script's declaration is its derivation's for a
   script in `/store`, which login reads as a program's; one outside
   `/store` has no deriver, so its `needs`, which its user approved at
   the shell, is its declaration, and the shell sends it with the run
   request ([shell.md L19](shell.md#l19-scripts)). Login makes the
   script a control channel of its own from the session's, through
   `NET_CONTROL`, with the rules its declaration names and at least one
   client reserved from the session's, and gives the programs the
   script runs their grants on that channel. A program a script runs
   therefore reaches no more of the network than the script declared,
   and a script that names no rules gets no channel, and its programs
   no grant.

Until login reads declarations, it gives programs no grant, and a
program run from the shell of [accounts.md A8](accounts.md#a8-the-shell)
has no network.

- **Why:** each holder's rules are read from a recipe before anything
  runs (principle 7), each control channel narrows the one it came
  from, as handle rights narrow (O5), and a channel's clients are
  reserved from its parent's, as a job's limits are
  ([objects.md O6](objects.md#o6-no-ambient-authority)). Login gives a
  program exactly its declaration because it reads the declaration
  itself, and can give no more than the session's channel holds. The
  service checks every grant against a channel it serves, so it never
  has to trust a handle passed to it. Each program is a client of its
  own, so it cannot take another's share
  ([N18](#n18-limits-on-clients)).
- **Why the shell holds no grant:** no built-in uses the network, and
  every program and script gets its grant from login on the session's
  channel, so a grant in the shell would be idle authority, the
  session's listen rules among it, in the process that parses
  everything typed; and each shell, the HUD's and every terminal's
  included, would take one of the session's clients.
- **Why outbound only by default:** the recipe states each account's
  network as it states the rest of the system, and an account it says
  nothing about can reach out but not be reached. Each program is still
  bounded by its own declaration, so a session's rules are the ceiling,
  not what its programs get.
- **Cost:** login holds the network authority of every open session
  ([N17](#n17-the-threat-model)); at most three sessions hold the
  network at once, each with four programs; a program
  that declares no rules cannot use the network, even to reach a server
  its user names on its command line; the system recipe names each
  account's rules, and an account it names none for neither listens nor
  reaches a local program over loopback.
- **Not taken:**
  - The shell deriving each program's grant from the session's and
    sending it with its run request, for login to make a client of.
    The service could not tell whether the channel login passed it was
    a grant it serves: each endpoint is an object of its own
    (`kernel/object/channel.c`), and `handle_info` reports a handle's
    own object, not its peer
    ([objects.md O4](objects.md#o4-handle-operations)), so a shell could
    pass a channel of its own making and answer for it. And the shell,
    not the launcher that reads the declaration, would choose the rules.
  - A grant built from the user's command line, as the shell grants
    paths. Its cost is a rule language on the command line for the
    common case of a program that always talks to one service, which
    its recipe can say once.
  - A session-wide grant in the shell, `SESSION_NETWORK`, from the
    session's channel. Its cost is the idle authority above, and a
    client for each shell, which would leave a session with the HUD
    and one terminal two clients for its programs.
  - No network for an account the recipe names none for. Its cost is a
    new account that reaches nothing until an administrator changes the
    recipe. Everything for every session, listening and loopback
    included; its cost is any account's programs open to the network,
    within what they declare, without the recipe saying so.

## Replay

### N16. Replay

**Decided,** as a refinement of [replay.md](replay.md), whose model
the outside world strains: a peer on the network is outside every
recording and behaves as it pleases.

1. **A program's network inputs are messages.** Replies to connects,
   accepts and echoes, and every byte a program reads, arrive inline in
   messages ([N13](#n13-the-socket-protocol)), which the kernel records
   ([replay.md](replay.md#inputs-through-the-kernel)). Replaying the
   program needs neither the service nor the network: the outside
   world's nondeterminism is captured where it entered the recorded
   process.
2. **What it sent is determined, not recorded.** A program's writes are
   outputs, functions of its inputs, which replay reproduces, so a
   recording fixes every byte the program sent and to whom, since the
   connect requests name the hosts.
3. **Replay sends nothing.** A replayed program's requests go to the
   replay, never to the service, so replaying a session that connected
   somewhere does not connect again.
4. **What it could reach is on record apart from replay.** Its grant's
   rules are exactly its derivation's declaration, which login read and
   gave it, or for a program a script outside `/store` ran, within the
   `needs` its user approved ([N15](#n15-declared-authority)), so a
   reader learns them from the derivation. Replay itself cannot ask the
   service a grant's rules, since no service runs under it.
5. **No socket data reaches a program through shared memory.** That
   is why data is inline: under replay.md, "no section a recorded
   process maps may be writable by a process outside the recording"
   ([Shared memory](replay.md#shared-memory)), and the service writes
   what a program receives.
6. **The service can be recorded once frames are messages.** Its inputs
   are messages, `clock_monotonic` and `random_read`, all recorded
   ([N9](#n9-one-thread-one-port-time-and-randomness)), but for frames,
   which the device writes into the section by DMA: the shared memory
   replay.md excludes, and the device memory it records only at a
   driver's message boundary ([Devices](replay.md#devices)). A
   recordable service needs a driver mode that copies each received
   frame, at most 1,514 bytes, into its `FRAME_RECEIVED` message. That
   mode is not in v1.
7. **A recording is as sensitive as what was read.** A program that
   speaks TLS decrypts in its own process ([N19](#n19-tls)), so its
   recording holds the ciphertext it read and the random bytes its keys
   came from, from which replay recovers the plaintext: a recording is
   "as sensitive as the session it records"
   ([Recordings](replay.md#recordings)).

- **Why:** the program boundary is where principle 8 needs an account,
  and messages already are what the kernel records, so the network adds
  nothing the kernel must learn
  ([objects.md O1](objects.md#o1-admission-to-the-core)).
- **Cost:** a recorded program's network data takes the inline path
  ([N13](#n13-the-socket-protocol)); recording the service waits on a
  driver mode.
- **Not taken:** recording at the wire, each frame in and out. Its cost
  is a recording of the whole stack's traffic for any one program's, and
  replay of a program that needs the service and its TCP state to turn
  frames back into the bytes the program read.

## Threats and limits

### N17. The threat model

**Decided.** What each part can do if it is compromised, and what a
hostile peer can do.

| Who | Can | Cannot |
|---|---|---|
| The driver | Read, change, drop and inject every frame, as an attacker on the path between the machine and its network can; without an IOMMU, read and write all memory by DMA, which is the whole system ([ipc.md I8](ipc.md#i8-dma)); stop the network | With an IOMMU, reach any memory but its pins; use any grant, read any socket's bytes but as they cross the wire, or reach any file |
| The service | Everything the network allows: read and change every program's data that is not encrypted end to end, connect anywhere, listen anywhere, ignore every grant, answer names falsely | Reach a file, the store or a device, or any process's memory; read data a program encrypts in its own process ([N19](#n19-tls)) |
| The resolver | Answer names with false addresses | Reach anything but the name servers' port 53; learn which program asked |
| Login | Give any program it starts the rules of any open session, up to what the system recipe gives sessions, within its clients | Exceed that, or reach a socket; it holds control channels, not grants |
| A program | What its grant covers, within its client's limits ([N18](#n18-limits-on-clients)) | Send a frame or a header it did not ask for: the service builds every header, and v1 has no raw sockets; see another program's traffic |
| A peer on the link | Claim any address with ARP, which RFC 826 does not authenticate, and so take traffic meant for another host; answer DHCP first, and so name the router and the name servers; flood the service with frames | Exceed the service's fixed tables ([N18](#n18-limits-on-clients)) |
| A peer anywhere | Guess at off-path attacks: forged resets, SYNs, data and ICMP errors, and forged DNS answers; flood a listener with SYNs | Succeed beyond the odds that unpredictable sequence numbers (RFC 6528), ports (RFC 6056), RFC 5961's checks, RFC 5927 4.1's checks of an ICMP error's TCP sequence number and RFC 5452's matching rules leave |

The service is the part an attacker reaches first: every frame from the
network is parsed there. Its parsers copy what they parse out of shared
memory first ([N3](#n3-buffers-ownership-and-dma)), check every length
against the bytes held before reading past it, recurse nowhere, and use
fixed tables with named bounds ([N18](#n18-limits-on-clients)).

Small TCP/IP stacks have a record of exactly these flaws. CERT/CC's
note on AMNESIA:33 describes "33 memory related vulnerabilities" in
uIP, Contiki, picoTCP, FNET and Nut/OS, "stemming from improper memory
management" (VU#815128); Forescout's NAME:WRECK disclosure describes
"nine vulnerabilities affecting four popular TCP/IP stacks (FreeBSD,
Nucleus NET, IPnet and NetX)", which "relate to Domain Name System
(DNS) implementations". The second is why the resolver is a process of
its own ([N11](#n11-names-and-the-resolver)); the first, why every
parser is checked on malformed input on the host
([N21](#n21-the-ci-test-plan)).

- **Why this split of trust:** the parts that face the network hold the
  least they can: the service no device and no file, the resolver
  almost nothing, the driver no grant. What remains is what TCP/IP
  itself gives an attacker on the path, which only cryptography in the
  endpoints removes ([N19](#n19-tls)).
- **Cost:** without an IOMMU, a compromised driver is a compromised
  system, as for every DMA driver until IOMMU support exists (I8); a
  compromised service sees every byte that is not encrypted end to end.
- **Not taken:** a service per program, each its own stack. Its cost
  is one address and one ARP and DHCP state per program, or a second
  layer to share them, and a frame from the network still parsed by
  something that holds every program's frames.

### N18. Limits on clients

**Decided.** The service divides what it holds among its clients, as
[storage.md F17](storage.md#f17-limits-on-clients) divides a service's
channels, so that no client can take what another holds.

1. **A client** is a grant that starts one, by `NET_GRANT` on a control
   channel, with every channel opened or derived through it. A program
   that derives a grant to pass on shares its own limits with the
   receiver.
2. **Clients.** There are at most `NET_CLIENTS` (16) at once. Each
   control channel holds a number of them, reserved from the channel it
   came from by `NET_CONTROL`, at least 1, as a job's limits are reserved
   from its parent's (O6), and a `NET_GRANT` past its channel's number
   fails with `ERR_NO_RESOURCES`. The root keeps `NET_SYSTEM_CLIENTS`
   (4) for system services ([N15](#n15-declared-authority)), so no
   session can take a system service's place. Since each control
   channel reserves at least one client, there are never more control
   channels than clients.
3. **Channels.** Each client has at most `NET_CLIENT_CHANNELS` (8) open:
   grants, streams, listeners, datagram and echo sockets, the endpoints
   of accepts not yet answered, and the connections waiting in its
   listeners' backlogs. A request past it fails with
   `ERR_NO_RESOURCES`.
4. **Rules.** A grant or control channel holds at most `NET_RULES_MAX`
   (16) rules.
5. **Buffers.** Each stream has a receive buffer of
   `NET_RECEIVE_BUFFER` (32 KiB), which bounds the window it advertises,
   and a send buffer of `NET_SEND_BUFFER` (32 KiB). Each datagram socket
   keeps at most `NET_DATAGRAMS` (8) datagrams unread, and drops newer
   ones.
6. **Requests.** The service holds at most `NET_SOCKET_REQUESTS` (3)
   requests of one socket unanswered: a read, a write and one other.
   While it holds that many it reads no more from that channel, so the
   rest wait in the queue the client sized when it created the channel,
   which [ipc.md I1](ipc.md#i1-channels), rule 5, charges to the
   client's job.
7. **Listeners.** Each holds at most its `backlog`, no more than
   `NET_BACKLOG_MAX` (4), connections that have completed their
   handshake and wait for an accept, and at most `NET_HALF_OPEN` (8)
   that have not, replacing the oldest of those when a new SYN comes,
   one of RFC 4987's common defenses (3.4).
8. **TIME-WAIT.** A connection its program closed first stays in
   TIME-WAIT for 4 minutes (MUST-13), holding no buffer. The service
   keeps at most `NET_TIME_WAIT_MAX` (1,024) such connections, and a
   client at most `NET_CLIENT_TIME_WAIT` (256): a connect while its
   client holds 256, or the service 1,024, fails with
   `ERR_NO_RESOURCES`. A client's TIME-WAIT connections outlive its
   channels and count against the service until they end, but not
   against a client that has ended.
9. **What no client owns.** Frames from the network are bounded by the
   service's own tables: `ARP_ENTRIES` (64), the least recently used
   replaced, and `REASSEMBLY_MAX` (4) datagrams being reassembled, the
   oldest dropped for a new one. Each client has at most
   `NAME_CLIENT_QUERIES` (2) names waiting on the resolver.

The bounds fit the kernel. A process holds at most
`HANDLE_TABLE_CAPACITY` handles, 256 with a table of one page
(`kernel/object/object.h`), and a port as many watches
([ipc.md I4](ipc.md#i4-ports)): sixteen clients of eight channels are
128 handles, the control channels at most 16 more, the service's own
handles about ten, and a message the service reads can bring 64 more
(I1), 218 in all. The sockets' buffers take at most 16 times 8 times
64 KiB, 8 MiB, of the service's memory. The replies it has queued for a
socket hold at most 14 pages of kernel memory, 10 for a 32 KiB read and
2 each for two short replies, and 7 MiB for 128 sockets.

- **Why:** a limit all clients share lets one program that opens
  connections, or listens with a long backlog, refuse every other; a
  fixed share per client is how F17 and O6 reserve resources. A client
  is a grant from a control channel because the launcher, which decides
  what each program reaches (N15), is what starts one, and the control
  channels' reservations keep sessions from taking the system's
  places.
- **Cost:** a program with more than eight connections at once, a web
  browser among them, fails, and at most three sessions hold the network
  at once, until the limits grow with the kernel's handle table; a
  client can close at most 64 connections a minute itself, and all
  clients 256, without its connects failing; the kernel memory of the
  service's queued replies is charged to the service's job by I1, rule
  5, which this kernel does not enforce yet
  ([ipc.md I4](ipc.md#i4-ports), rule 3).
- **Not taken:** charging each client's sockets to its job. No call
  tells a server the job at a channel's other end, as F17 found.

### N19. TLS

**Decided:** TLS is a library in each program that
needs it, and which library is a later specification's to decide.

TLS is not the stack's. It lives in each program that
needs it, as a library from the program's closure, in the program's own
process: its keys and its plaintext never leave that process. The trust
anchors are a store path the program's derivation names; checking a
certificate's validity period needs the wall-clock time, which a
wall-clock service not yet specified will give
([storage.md F2](storage.md#f2-attributes) names it). Which library,
and whether nova-os writes its own, is the TLS specification's to
decide.

- **Why:** a program that encrypts in its own process trusts the
  service, the driver and the network for nothing but delivery
  ([N17](#n17-the-threat-model)). RFC 9293 makes the same point of TCP
  itself: it has "no built-in cryptographic capabilities", and
  applications "typically utilize ... upper-layer (e.g., IPsec) and
  upper-layer (e.g., TLS) protocols" (7).
- **Cost:** every program that speaks TLS carries the library in its
  closure, shared on disk by the store but mapped per process; each
  holds its own trust anchors' path.
- **Not taken:** a TLS service that terminates connections for
  programs. Its cost is one process holding every program's plaintext
  and keys, so a compromise of it is the compromise end-to-end
  encryption exists to prevent.

## Milestones

### N20. The order of work

**Decided.** Each step is demonstrable on its own, and the CI checks
it ([N21](#n21-the-ci-test-plan)).

**1. A link and a ping.** The driver ([N2](#n2-the-virtio-network-driver)
to [N4](#n4-the-frame-protocol)), its supervision
([N5](#n5-supervision)), and the service with Ethernet, ARP, IPv4, ICMP
and DHCP; grants with `connect icmp` rules and echo sockets. On a test
disk ([disk-layout.md](disk-layout.md#files)) the first process starts
a test program, `nettest.exe`, with a grant of `connect icmp 10.0.2.2`,
and reports it as a test of its own
([log.md T2](log.md#t2-test-reports)).

Done when:

- the service logs the address and prefix DHCP gave it, with the router
  and name servers where the server gave them (QEMU's, isolated as the
  CI isolates it, gives neither, [N21](#n21-the-ci-test-plan));
- `nettest` sends Echo Requests to 10.0.2.2 and reports each reply as
  a case;
- an echo to a host its grant does not cover, 10.0.2.3, fails with
  `ERR_ACCESS_DENIED` and sends nothing.

**2. TCP and grants.** UDP, TCP, the loopback interface, and the whole
socket protocol ([N13](#n13-the-socket-protocol)). `nettest`'s grant
gains `listen tcp * 7`, `listen tcp localhost 7`, `connect tcp
10.0.2.100 7` and `connect tcp localhost 7`.

Done when:

- `nettest` echoes on port 7 what a client on the host sends through
  QEMU's forwarding, a megabyte of random bytes returned byte for byte;
- `nettest` connects out to an echo service on the host and checks a
  megabyte returned;
- `nettest` connects to its own loopback listener and checks the
  same;
- a connect outside its grant, to 10.0.2.2 port 80, fails with
  `ERR_ACCESS_DENIED`.

**3. Names.** The resolver ([N11](#n11-names-and-the-resolver)) and
connects by name.

Done when:

- `nettest` connects by name to `localhost`, which the service answers
  itself, and its loopback echo passes;
- the resolver's parser passes the host check of
  [N21](#n21-the-ci-test-plan) on generated and malformed answers.

**4. What SSH needs.** An SSH server, which its own specification will
give, needs from the network a listen grant on port 22, declared in the
system recipe ([N15](#n15-declared-authority)); streams with Nagle's
algorithm off for interactive sessions (`NET_NO_DELAY`); and connections
that live for hours, which v1's TCP gives without keep-alives. Outside
the network it needs: a session for a user who did not log in at the
console, opened by the accounts service on a request from the server,
by password or by a public key the user's home holds
([accounts.md A6](accounts.md#a6-the-accounts-service)); the console's
session protocol carried over an SSH channel, which the shell already
speaks to login ([accounts.md A7](accounts.md#a7-the-session)); and the
SSH protocol's cryptography in the server's process, as TLS's is
([N19](#n19-tls)). Sessions' control channels and declared grants
([N15](#n15-declared-authority), items 4 to 6) come in this step, so a
remote session's programs hold the network its account is given, as a
console session's do.

- **Why this order:** each step needs the one before and proves one
  layer: the link and IP before TCP, TCP before names, which need it for
  truncated answers, and all of it before a service that others reach.
- **Cost:** programs get the network only at step 2, and by name only
  at step 3.
- **Not taken:** TCP first, over a static address. Its cost is a
  configuration path the system recipe would carry only for the
  milestone, where DHCP is what every network the tests run on offers.

### N21. The CI test plan

**Decided.**

The Boot job's machine (`ci/boot.py`, `DEVICES`) gains a network device
on QEMU's user network, in place of `-net none`:

```
-nic user,model=virtio-net-pci,restrict=on,hostfwd=tcp:127.0.0.1:PORT-:7,guestfwd=tcp:10.0.2.100:7-cmd:cat
```

where `PORT` is a free port boot.py takes on the runner. On the CI's
QEMU, Ubuntu 24.04's 8.2.2 with libslirp 4.7.0, that machine has the
following. QEMU 11.1.3 and libslirp 4.9.7, which Homebrew gives a Mac,
read the same in every file the README pins at both versions.

| What | Behaviour | Source |
|---|---|---|
| The device | `virtio-net-pci` on the root bus, transitional, 1AF4:1000 ([N2](#n2-the-virtio-network-driver)); `VIRTIO_NET_F_MAC` always offered; no checksum or segmentation offload over the user network, whose backend has no virtio header to carry them | QEMU `hw/virtio/virtio-pci.c`; `hw/net/virtio-net.c`, `virtio_net_get_features` |
| DHCP | 10.0.2.15 of 10.0.2.0/24, from 10.0.2.2, for 24 hours; with `restrict=on`, no router and no name server option | QEMU `net/slirp.c`, the defaults; libslirp `src/bootp.c`, `LEASE_TIME` and the options sent only `if (!slirp->restricted)` |
| Echo | 10.0.2.2 answers, from libslirp itself, restricted or not: "``ping``, aka. ICMP echo, to the local router (10.0.2.2) shall work" | libslirp `src/ip_icmp.c`; QEMU `docs/system/devices/net.rst` |
| Inbound | A connection to 127.0.0.1:`PORT` on the runner reaches port 7 of the guest's first DHCP address | `qemu-options.hx`, `hostfwd` |
| Outbound | Each connection the guest opens to 10.0.2.100 port 7 runs `cat`, "spawned for each connection", on the connection: an echo service with nothing listening on the runner | `qemu-options.hx`, `guestfwd`; libslirp `src/misc.c`, `fork_exec` |
| Isolation | "the guest will be isolated, i.e. it will not be able to contact the host and no guest IP packets will be routed over the host to the outside. This option does not affect any explicitly set forwarding rules": libslirp resets any other TCP connection and drops any UDP datagram but DHCP's and TFTP's | `qemu-options.hx`, `restrict`; libslirp `src/tcp_input.c`, `src/udp.c` |
| The peer | The guest's TCP ends in libslirp's, which relays to sockets on the runner; its `tcp_input.c` descends from 4.4BSD's, `@(#)tcp_input.c 8.5 (Berkeley) 4/10/94` | libslirp `src/tcp_input.c` |

For each boot of `test.img`, boot.py:

1. waits, beside the lines it waits for now, for the driver running, the
   service's address, `10.0.2.15/24`, and `nettest`'s cases for its Echo
   Replies from 10.0.2.2, its refused echo to 10.0.2.3 and its listener
   on port 7;
2. connects to 127.0.0.1:`PORT`, sends `NETTEST_BYTES` (1 MiB) of
   random bytes, and reads until they have all come back or the boot's
   deadline passes; a short, different or late echo fails the boot;
3. waits for `nettest`'s cases for its outbound and loopback echoes and
   its connect refused by its grant, and for its test to pass.

boot.py fails a boot on any line from nova-os that reports a failure
([log.md T1](log.md#t1-lines)), `nova-init: child restarted` among
them, so a restart of the driver, the service or the resolver in the
middle of a test fails it, and the lines that report an expected
refusal, `nettest`'s and the service's, say `refused`, never `failed`.

The Lint job builds the service's protocol code and the resolver's
parser for the runner, where they run with no kernel, since only the
event loop calls one ([N9](#n9-one-thread-one-port-time-and-randomness)).
A host check, `tools/check-net.c`, as `tools/check-masks.c` is one,
feeds them frames, DHCP replies and DNS answers, written by hand and
generated, malformed ones among them, and checks what they send and
what they refuse: lost and reordered segments for TCP's retransmission
and congestion control, overlapping and missing fragments for
reassembly, compression loops and lengths past the message for names.

A boot cannot look a name up across the network without leaving the
runner: with `restrict=on`, libslirp's DHCP names no name server, and
its `udp.c` drops a datagram to its own DNS address before it would
relay it (`if (slirp->restricted) { goto bad; }`), and its `tcp_input.c`
resets a connection to it. So step 3 of [N20](#n20-the-order-of-work)
is checked by the host check of the resolver's parser and by the boot's
connect to `localhost`, and no boot looks a name up across the network
in v1.

- **Why:** nothing leaves the runner but the two forwardings, so a test
  never fails because a network outside it is down, and nova-os's TCP
  is tested in both directions against an independent TCP, on every
  processor model and accelerator the Boot job runs. `cmd:cat` needs
  no server on the runner and no ordering between it and QEMU.
- **Cost:** the boot's peer is always libslirp's TCP over a link that
  loses, delays and reorders nothing, so TCP's recovery paths are
  tested only by the host check; a lookup over the network is not
  tested in a boot; `cat` must be on the runner's `PATH`.
- **Not taken:** `restrict=off`. The guest then reaches the Internet,
  and the runner's own loopback through 10.0.2.2, which libslirp's
  `src/socket.c` rewrites to the host's loopback address: a test of
  whatever the runner's network does that day. A `guestfwd` to a
  character device on a server boot.py runs: QEMU opens the device "on
  bootup" (`qemu-options.hx`, the example's comment), so the server
  must be listening before QEMU starts, and every connection shares the
  one device. A boot without `restrict=on` that looks a public name up
  and compares the answer with the runner's own: the cost of
  `restrict=off` above. Name servers the system recipe names, one
  reached over TCP only through a `guestfwd` to a responder boot.py
  runs; its cost is two features, name servers named in the recipe and
  lookups over TCP alone, added for a test before either is needed for
  its own sake.

## References

Sources other than specifications, pinned under [Network design
references](README.md#network-design-references), at the versions
named there:

- **QEMU** 8.2.2, Ubuntu 24.04's, and 11.1.3, Homebrew's:
  `qemu-options.hx`, `docs/system/devices/net.rst`, `net/slirp.c`,
  `hw/net/virtio-net.c` and `hw/virtio/virtio-pci.c`.
- **libslirp** 4.7.0, Ubuntu 24.04's, and 4.9.7, Homebrew's:
  `src/bootp.c`, `src/udp.c`, `src/tcp_input.c`, `src/ip_icmp.c`,
  `src/misc.c` and `src/socket.c`. Its files are BSD-3-Clause or MIT.
- **lwIP** 2.2.1: `COPYING`, `src/include/lwip/opt.h`.
- **picoTCP** 1.7.0 and **picoTCP-NG** 2.1: `COPYING`.
- **FreeBSD** 15.1: `sys/netinet/tcp_input.c`, `sys/netinet/ip_input.c`,
  `sys/netinet/in_cksum.c`.
- **smoltcp** 0.14.0: `LICENSE-0BSD.txt`, `README.md`,
  `src/iface/interface/mod.rs`.
- **uIP** 1.0: `uip/uip.c`.
- **MINIX 3**: `minix/net/lwip/lwip.c`, `minix/lib/liblwip/lib/lwipopts.h`,
  and the commit that added the service.
- **Fuchsia**: the Netstack3 roadmap (2021, updated 2025-02-28) and
  Contributing to Netstack3 (updated 2025-03-22).
- **CERT/CC** VU#815128, AMNESIA:33 (revised 2025-06-06), and
  **Forescout**'s NAME:WRECK disclosure (April 12, 2021).
- **Private Network Access** (WICG, Draft Community Group Report,
  26 September 2024) and **Local Network Access** (WICG, Draft Community
  Group Report, 7 August 2026), by section.
- **POSIX**: `<netinet/in.h>`, The Open Group Base Specifications
  Issue 8 (IEEE Std 1003.1-2024).
