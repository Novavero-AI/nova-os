# Input

How a key press or a pointer's motion reaches a process: the virtio
input driver (`drivers/virtio_input.c`), the messages it sends its
client, the compositor or the console (`include/input_protocol.h`), its
report to the platform driver
(`drivers/virtio_input.h`), and how the platform driver, the first
process, starts and supervises it and its client (`init/platform.c`).

Specifications are cited by the versions in [README.md](README.md):
"VIRTIO 1.4" by section, and "evdev" for the Linux input documents
pinned under [Devices](README.md#devices), which VIRTIO 1.4 5.8 defers
to for the meaning of an event.

## K1. The driver

**Decided.**

The platform driver starts `\nova\virtinp.exe` for every virtio input
function it finds, PCI 1AF4:1052 (VIRTIO 1.4 4.1.2, 5.8.1), each in a
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
   `EV_KEY` bitmap, whose bit n % 8 of byte n / 8 is key code n, and for
   a device that has `EV_ABS`, the range of `ABS_X` and of `ABS_Y`
   (`VIRTIO_INPUT_CFG_ABS_INFO`). It reads at most the 128 bytes of the
   union, whatever `size` claims. It logs the name, the types and how
   many key codes the device has, and refuses a device with none.
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
   event (2.7.8.3). The event is copied out of the buffer once. On a
   keyboard it is sent (K2) if its type is `EV_KEY`, its code is in the
   bitmap and its value is 0, 1 or 2; anything else, `EV_SYN` included,
   is ignored (5.8.6.1). On a pointer, a device that has `EV_ABS` or
   `EV_REL`, its `EV_KEY` events within the bitmap, its `EV_ABS` events
   of `ABS_X` and `ABS_Y` and its `EV_REL` events of `REL_X`, `REL_Y`
   and `REL_WHEEL` are collected up to each `SYN_REPORT` and sent as one
   pointer report (K2); anything else is ignored. The buffers go back
   to the device once the pass has sent their events.

The bitmap's bit order is not in VIRTIO 1.4; QEMU's devices were
observed to use this one. VIRTIO 1.4 5.8.4 says a string has no NUL
terminator, but QEMU 11.1's devices were observed to count one in the
name's `size`, so the name ends at its first NUL.

- **Why a driver for every function:** a keyboard and a tablet are
  separate virtio input functions, each with its own queues and
  configuration, so each gets a driver that can fail and restart alone.
- **Why only these events:** a key press, a tablet's position, a
  mouse's motion, and a pointer's buttons and wheel, are what the first
  set needs ([display.md D10](display.md#d10-the-pointer)); the
  device's `EV_LED` and `EV_REP` (QEMU's keyboard has both) are for the
  status queue and for software autorepeat, which nothing does.
- **Why check the codes against the bitmap:** the device states what it
  sends, so a code outside that set is one the driver does not
  recognize, and the client never receives a code the device did not
  declare.

## K2. Key events and pointer reports

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

**Pointer reports.** A pointing device's update is several events closed
by `EV_SYN` `SYN_REPORT`, which a device should buffer or drop whole
(VIRTIO 1.4 5.8.6.2; evdev, `SYN_REPORT`). The driver sends each update
as one message, transaction 0, on the client's endpoint, in the same
queue as key events: the axes that changed, with their values; the
buttons that changed, pressed or released; a mouse's motion, `REL_X`
and `REL_Y`, as the device's deltas summed over the update
([display.md D10](display.md#d10-the-pointer), item 6); the wheel's
movement, as `REL_WHEEL` gives it; and the time it took the
`SYN_REPORT` from the used ring (`include/input_protocol.h`). A report
holds at most `INPUT_REPORT_BUTTONS` (8) changes of buttons, and one of
each; an update that changes more, or one button twice, is sent as more
than one report, each ended at the change that would not fit, at the
time it was taken. An `EV_SYN` `SYN_DROPPED` drops
the events collected since the last report and every event up to and
including the next `SYN_REPORT`, as evdev asks of a client (evdev,
`SYN_DROPPED`).

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
- **Why a pointer's update as one report:** an absolute position is two
  axes, and a pointer moved in two messages would be seen for a moment
  at a place it never was
  ([display.md D10](display.md#d10-the-pointer)).
- **Cost:** a message for every press and every release, each holding
  two pages of kernel memory while it is queued, one for the message and
  one for its bytes (`kernel/object/channel.c`); two message formats on
  one kind of endpoint.
- **Not taken:** a message per evdev report for a keyboard, the events
  up to an `EV_SYN`. Its cost is a variable-length format for a
  keyboard that reports one key at a time. A pointer's events one
  message each, as keys are; its cost is the half-moved pointer above,
  and twice the messages for every motion.

## K3. The report and the client

**Decided.**

Once the device is live the driver creates the client's channel and
writes one message on its bootstrap channel: 4 bytes, a transaction ID
of 0, and one handle, the client's endpoint, with `READ`, `WAIT` and
`TRANSFER`. It then closes the bootstrap channel. The platform driver
takes the report only if it is exactly that; otherwise it logs it and
closes the channel, which destroys the endpoint, and the driver's watch
finds the client gone and stops it (K2). The driver's first message on
the client's endpoint describes the device: a keyboard or a pointer,
and for a pointer the range of each absolute axis (K1); key events or
pointer reports follow it.

Where the first process starts no compositor, the platform driver starts
the client, the console, `\nova\console.exe`, in the root job when a
driver first reports, and keeps its end of the client's bootstrap
channel for as long as it supervises the client. Every message on that
channel is 4 bytes and a transaction ID of 0: the first, the bootstrap
message, carries the handles the console needs ([console.md
C2](console.md#c2-starting-the-console)); each after it carries one
handle, an endpoint the driver reported, with `READ` and `WAIT` alone,
one for the driver's first report and one for each report after a
restart (K4). The console reads every endpoint it holds, up to
`INPUT_DEVICES_MAX` (4): the description first, then each key event as
a key typed at it ([console.md C3](console.md#c3-keys)) and each pointer
report as [console.md C6](console.md#c6-the-pointer) gives. It closes
an endpoint once its driver has closed it, takes each new one from the
channel when one comes, and exits once the platform driver has closed
the channel.

While a compositor runs, it is every input driver's client in the
console's place ([display.md D2](display.md#d2-who-holds-the-display)):
the platform driver starts no console, and hands the compositor each
endpoint a driver reports, on the channel it keeps to the compositor,
as it hands them to the console; an endpoint reported before the
compositor starts waits in the platform driver until it does. Until
display.md's phase 4, the platform driver starts the console beside
the compositor and hands it no endpoint while the compositor runs, as
[display.md D19](display.md#d19-the-build-plan)'s phase 3 arrangement
gives.

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
- **Why a description first:** a client tells a keyboard's endpoint
  from a pointer's, and maps an absolute axis onto the screen, only
  from what the device declares, which the driver alone reads.
- **Not taken:** the platform driver creating the channel and passing an
  end in the driver's bootstrap message. Its cost is a bootstrap message
  that differs for one driver, and an endpoint that exists before
  anything can serve it.

## K4. Supervision

**Decided.**

The platform driver supervises the driver and its client for the life
of the system, as [ipc.md I10](ipc.md#i10-driver-failure-and-restart)
describes. It waits on one port, with a watch of each one's process for
`TERMINATED` and of each driver's bootstrap channel for its report.
Startup waits for each driver's report, or for it to be given up; the
first process then reports that its startup is done
([boot-protocol.md](boot-protocol.md#startup-done)), and the fade into
the desktop does not wait for a key.

When the driver ends, whatever its exit code, or does not start, the
platform driver stops the function as I10 gives, clearing Bus Master
Enable, resetting the device and releasing the initiator's quarantine,
and starts the driver again under I10's restart policy, except as the
paragraph on a client's end below gives. The client,
which saw `PEER_CLOSED`, takes the endpoint the new driver reports
(K3). When the client ends it is started again under the same policy,
and waits for an endpoint; its driver, whose watch saw the old endpoint
close, ends and is restarted with it, without counting a restart of its
own and logging `reason="its client restarted"`, so that a client's
failure soon after the driver's own restart does not give the driver
up. A driver whose client has been given up is not restarted, since
nothing would read its endpoint.

The kernel closes a process's handles, which ends the drivers on its
endpoints, before it asserts the process's `TERMINATED`
([objects.md O10](objects.md#o10-object-lifetime)), so the
platform driver can read a driver's end before its client's. It decides a
driver's end from the end's facts alone, never from which end it read
first. A driver that ends with `VIRTIO_INPUT_EXIT_DONE`, the exit code of
a driver whose client has closed its endpoint, while its client is the
one its endpoint went to and has not been started again, waits, logging
`child waiting` with `reason="its client's end is not read"`. When the
client's end is read the driver is decided again: started again for the
client's restart or for the client that took its place, without counting
a restart, or left down for a client given up. An endpoint that a client
whose handles are closed cannot take is closed by the platform driver,
which ends its driver the same way. Any other end is the driver's own,
and the driver is started again under I10's restart policy.

When the first process gives the compositor up, the platform driver
makes the console the client instead
([display.md D3](display.md#d3-the-compositors-supervision-its-restart-and-what-a-crash-shows)):
it starts the console in direct mode and starts every input driver
again for it, without counting a restart, so the console takes a fresh
endpoint from each (K3). Here the client is replaced, not lost. Until
display.md's phase 4 the console already runs, beside the compositor,
and is made the client as
[display.md D19](display.md#d19-the-build-plan)'s phase 3 arrangement
gives; a driver started again for a new client logs
`reason="its client changed"`.

- **Why:** a driver can fail at any time, not only while it starts;
  unsupervised, one that died after startup would leave its device
  live, bus mastering on and its MSI-X entry naming a vector the kernel
  frees ([ipc.md I7](ipc.md#i7-interrupts)), and the keyboard dead
  until the next boot.
- **Why a driver waits for its client's end:** the driver's end is its
  own or its client's, and only the client's end says which; the kernel
  can queue that after the driver's, so a decision at the driver's end
  alone gives a different answer for each order the two are read in.
- **Why the console after the compositor:** a machine whose compositor
  fails keeps its keyboard and pointer at a console, where its owner can
  log in, read the log and fix it.
- **Cost:** a restart loses the events queued for the old endpoint and
  any the device took while no driver ran; a client that ends takes its
  driver down with it; a compositor given up loses the events queued for
  it. A client that closes an endpoint and keeps running, as the console
  does for a message out of place, leaves that driver waiting until the
  client ends or is replaced: the platform driver cannot tell the close
  from the client's end until it reads that end.
- **Not taken:** keeping the driver running across its client's end and
  having it serve a new endpoint. Its cost is a second protocol between
  driver and supervisor for endpoints after the report, where the
  restart reuses the one there is. Starting a driver that ends for a
  client as it was again at once, counting the restart: it counts the
  client's end as the driver's own whenever the driver's is read first,
  and gives the driver up at the client's next failure within the
  restart period. Waiting a set time for the client's end, which a
  loaded machine overruns. Asking the kernel to show a process ending
  before its handles close, which changes O10 for this one reader.
