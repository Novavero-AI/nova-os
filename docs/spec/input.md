# Input

How a key press reaches a process: the virtio input driver
(`drivers/virtio_input.c`), the messages it sends its client
(`include/input_protocol.h`), its report to the platform driver
(`drivers/virtio_input.h`), and how the platform driver, the first
process, starts and supervises it and its client (`init/platform.c`).

Specifications are cited by the versions in [README.md](README.md):
"VIRTIO 1.4" by section, and "evdev" for the Linux input documents
pinned under [Devices](README.md#devices), which VIRTIO 1.4 5.8 defers
to for the meaning of an event.

## K1. The driver

**Decided.**

The platform driver starts `\nova\virtinp.exe` for the first virtio
input function it finds, PCI 1AF4:1052 (VIRTIO 1.4 4.1.2, 5.8.1), in a
job of its own, with the bootstrap message every PCI device driver gets
(`drivers/pci.h`). The driver:

1. Resets the device, turns bus mastering on and accepts VERSION_1,
   ACCESS_PLATFORM and ORDER_PLATFORM; an input device has no feature
   bits of its own (5.8.3).
2. After FEATURES_OK, before which it may not write the configuration
   (3.1.1, step 4), reads the device-specific configuration (5.8.4): it
   writes `select` and `subsel`, then reads `size` and the data within
   one configuration generation (2.5.1). It reads the name
   (`VIRTIO_INPUT_CFG_ID_NAME`), which event types below 32 the device
   has (`VIRTIO_INPUT_CFG_EV_BITS` with a nonzero `size`), and the
   `EV_KEY` bitmap, whose bit n % 8 of byte n / 8 is key code n. It
   reads at most the 128 bytes of the union, whatever `size` claims. It
   logs the name, the types and how many key codes the device has, and
   refuses a device with none.
3. Sets up the event queue, 64 device-writable buffers of 8 bytes, one
   per descriptor, on MSI-X table entry 0, and leaves the status queue
   unused (5.8.2). It gives every buffer to the device, sets DRIVER_OK
   and only then notifies (3.1.1).
4. Creates the client's endpoint and registers a watch of it for
   `PEER_CLOSED` on the port its interrupt is bound to, with
   `object_wait_async` ([ipc.md I4](ipc.md#i4-ports)), then reports
   (K3).
5. On each interrupt packet ([ipc.md I7](ipc.md#i7-interrupts))
   acknowledges it and takes the buffers the used ring returned, in
   order (2.7.14). More buffers returned than the queue holds, one the
   device does not hold, or the same one twice, ends the driver as a
   broken device. A buffer with fewer than 8 bytes written holds no
   event (2.7.8.3). The event is copied out of the buffer once, and sent
   (K2) if its type is `EV_KEY`, its code is in the bitmap and its value
   is 0, 1 or 2; anything else, `EV_SYN` included, is ignored (5.8.6.1).
   The buffers go back to the device once the pass has sent their
   events.

The bitmap's bit order is not in VIRTIO 1.4; QEMU's devices were
observed to use this one. VIRTIO 1.4 5.8.4 says a string has no NUL
terminator, but QEMU 11.1's devices were observed to count one in the
name's `size`, so the name ends at its first NUL.

- **Why only `EV_KEY`:** a key press is what the first set needs; the
  device's `EV_LED` and `EV_REP` (QEMU's keyboard has both) are for the
  status queue and for software autorepeat, which nothing does yet.
- **Why check the codes against the bitmap:** the device states what it
  sends, so a code outside that set is one the driver does not
  recognize, and the client never receives a code the device did not
  declare.

## K2. Key events

**Decided.**

Each key event is one message of 24 bytes and no handles on the
client's endpoint, in the order the device returned them:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `transaction`: 0, since an event answers no call ([ipc.md I3](ipc.md#i3-synchronous-call-and-asynchronous-queues)) |
| 4 | 2 | `type`: `EV_KEY` (1) |
| 6 | 2 | `code`: the evdev key code, as the device gave it |
| 8 | 4 | `value`: 0 released, 1 pressed, 2 repeated while held (evdev, `EV_KEY`) |
| 12 | 4 | Reserved, 0 |
| 16 | 8 | `time`: the monotonic clock in nanoseconds (`clock_monotonic`) when the driver took the event from the used ring |

The endpoint queues up to 32 messages. When it is full the driver waits
for `WRITABLE`, and gives no buffer back meanwhile, so the device runs
out of buffers and may drop events (5.8.6.2) instead of the kernel
queueing them without bound. The driver learns that the client has
closed the endpoint from its watch's signal packet, at once and with
no key pressed, or from a send that fails with `ERR_PEER_CLOSED`,
whichever comes first; it then resets the device, releases its pin
(3.3.1) and exits.

- **Why evdev's fields as they come:** VIRTIO 1.4 fills `type`, `code`
  and `value` according to evdev (5.8.6), so the client reads the same
  vocabulary the device writes, with no table between them.
- **Why the time of taking:** one interrupt packet stands for every
  buffer used until the driver acknowledges it, so the packet's time
  ([ipc.md I4](ipc.md#i4-ports)) would date events the device wrote
  after the interrupt fired before they happened.
- **Why a watch:** the driver waits on its port for interrupts, and a
  watch puts the client's close on the same port, so the device is
  reset as soon as nobody reads it rather than at the next key.
- **Cost:** a message for every press and every release, each holding
  two pages of kernel memory while it is queued, one for the message and
  one for its bytes (`kernel/object/channel.c`).
- **Not taken:** a message per evdev report, the events up to an
  `EV_SYN`. Its cost is a variable-length format for a keyboard that
  reports one key at a time.

## K3. The report and the client

**Decided.**

Once the device is live the driver creates the client's channel and
writes one message on its bootstrap channel: 4 bytes, a transaction ID
of 0, and one handle, the client's endpoint, with `READ`, `WAIT` and
`TRANSFER`. It then closes the bootstrap channel. The platform driver
takes the report only if it is exactly that; otherwise it logs it and
closes the channel, which destroys the endpoint, and the driver's watch
finds the client gone and stops it (K2).

The platform driver starts the client, `\nova\inputlog.exe`, in the root
job when the driver first reports, and keeps its end of the client's
bootstrap channel for as long as it supervises the client. Every
message on that channel is 4 bytes, a transaction ID of 0, and one
handle: the first, the bootstrap message, carries the debug log; each
after it carries an endpoint, as the driver reported it, one for the
driver's first report and one for each report after a restart (K4).
The client logs each key event on its endpoint as `nova-input-log: key`
*code* `pressed`, `released` or `repeated at` *time* `ns`. Once the
driver has closed the endpoint and every event queued there is logged,
the client takes the next endpoint from the channel, waiting for one,
and exits once the platform driver has closed the channel.

- **Why the driver creates the endpoint:** it is the driver's service,
  passed to clients through its supervisor, as
  [ipc.md I10](ipc.md#i10-driver-failure-and-restart) has a supervisor
  publish a driver; every PCI device driver keeps the same bootstrap
  message.
- **Why the client keeps its channel:** a restarted driver serves a new
  endpoint, and the client's channel to its supervisor is how it gets
  one, the way the driver's report reaches the supervisor, until the
  namespace publishes the device by name
  ([ipc.md I10](ipc.md#i10-driver-failure-and-restart), step 4).
- **Not taken:** the platform driver creating the channel and passing an
  end in the driver's bootstrap message. Its cost is a bootstrap message
  that differs for one driver, and an endpoint that exists before
  anything can serve it.

## K4. Supervision

**Decided.**

The platform driver supervises the driver and its client for the life
of the system, as [ipc.md I10](ipc.md#i10-driver-failure-and-restart)
describes. It waits on one port, with a watch of each one's process for
`TERMINATED` and of the driver's bootstrap channel for its report.
Startup waits for the report, or for the driver to be given up; the
first process then reports that its startup is done
([boot-protocol.md](boot-protocol.md#startup-done)), and the fade into
the desktop does not wait for a key.

When the driver ends, whatever its exit code, or does not start, the
platform driver stops the function as I10 gives, clearing Bus Master
Enable, resetting the device and releasing the initiator's quarantine,
and starts the driver again under I10's restart policy. The client,
which saw `PEER_CLOSED`, takes the endpoint the new driver reports
(K3). When the client ends it is started again under the same policy,
and waits for an endpoint; its driver, whose watch saw the old endpoint
close, ends and is restarted with it, without counting a restart of its
own, so that a client's failure soon after the driver's own restart
does not give the driver up. A driver whose client has been given up
is not restarted, since nothing would read its endpoint.

- **Why:** a driver can fail at any time, not only while it starts;
  unsupervised, one that died after startup would leave its device
  live, bus mastering on and its MSI-X entry naming a vector the kernel
  frees ([ipc.md I7](ipc.md#i7-interrupts)), and the keyboard dead
  until the next boot.
- **Cost:** a restart loses the events queued for the old endpoint and
  any the device took while no driver ran; a client that ends takes its
  driver down with it.
- **Not taken:** keeping the driver running across its client's end and
  having it serve a new endpoint. Its cost is a second protocol between
  driver and supervisor for endpoints after the report, where the
  restart reuses the one there is.
