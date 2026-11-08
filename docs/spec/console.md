# Console

The console: a user-mode service that draws a text panel over the
desktop background, reads the keyboard and the pointer, and serves
programs text output and line input (`console/`). This document gives
how it gets the screen from the kernel through the first process, how
the first process starts it and gives it sessions (`init/console.c`),
what the keyboard does in it, the session protocol a program speaks
(`include/console_protocol.h`), what it draws, and what it logs of the
pointer.

Specifications are cited by the versions in [README.md](README.md):
"evdev" is the Linux input documents pinned under
[Devices](README.md#devices), "QEMU" the device models pinned there,
and "SDM" the Intel manual's Volume 3.

## C1. The display handoff

**Decided.**

The kernel draws the boot screen and fades it into the desktop
background ([boot-protocol.md](boot-protocol.md#startup-done)). Once the
fade's last frame is drawn, it closes its end of the first process's
bootstrap channel and never draws again. The first process takes
`PEER_CLOSED` on its endpoint as the display's release.

The screen's first frame after the release is prepared before it,
while the boot screen's spinner turns:

1. The kernel starts the first process, draws Limb offscreen into
   memory sections and hands them to the process on its bootstrap
   channel, labelled with the background they hold and its size
   ([boot-protocol.md](boot-protocol.md#startup-done)).
2. The first process, its own startup done, takes the background and
   sends it, with the display it is to give, to the console started by
   then, on the console's control channel (C2).
3. The console prepares its panel from the background where the label
   matches that display (C5), closes the sections, and answers on the
   control channel; where the label does not match, it answers at
   once.
4. The first process waits for the answer, or for the control channel
   to close, and then reports its startup done.
5. The kernel stops the spinner, fades the boot screen into Limb and
   releases the display; the first process sends the display to the
   console, whose first draw is a copy of its prepared panel.

The kernel holds its handles to the sections until the fade's last
frame. The first process holds its own from the kernel's message until
it sends them on, and closes them at once where no console runs or the
display goes to the type specimen. The console holds them until its
panel's backdrop is made. No handle carries `WRITE`, and the console
maps them only to read; once all three have let go, after the fade,
the pages return to the page allocator, and a console restarted later
computes its panel's Limb (C5).
While a compositor runs, the first process holds the sections through
the release and gives them to it as its wallpaper
([display.md D2](display.md#d2-who-holds-the-display)).

The bootstrap message's platform record describes the display in a
`BOOTSTRAP_DISPLAY` (`include/native_abi.h`): the physical address of
its first pixel, its width and height in pixels, its pixels per scan
line and its pixel format, as nova-boot reported them in `BOOT_INFO`.
After the release the first process creates, with `resource_create` on
its root MMIO resource, a resource covering exactly the pages the
framebuffer's `stride` x `height` pixels lie on, and sends it with the
record to the console (C2), or, while a compositor runs, to the
compositor ([display.md D2](display.md#d2-who-holds-the-display)). The
console makes write-combining sections over it, each at most
`SECTION_MAX_SIZE`, and maps them
([ipc.md I6](ipc.md#i6-device-memory-and-io-ports)). One process at a
time holds a resource over the framebuffer: the compositor while it
runs, and the console before a compositor exists and once the first
process has given the compositor up
([display.md D3](display.md#d3-the-compositors-supervision-its-restart-and-what-a-crash-shows)).
So after the release only that process and the first process, which
holds every device's authority, can draw. On a boot disk whose
`\nova\` holds a file named `SPECIMEN`, the first process gives the
display to the type specimen from `/store` (`test/specimen.c`) in the
console's place, after its launch message on its bootstrap channel,
and the console never gets it. A console that takes the
display from a compositor draws Limb over the whole screen and then its
panel, so nothing of the compositor's last frame stays.

- **Why the kernel's close:** the bootstrap channel already joins the
  kernel and the first process and nothing else, and its close is a
  signal the first process can wait for, so the release needs no new
  message or object. The report and the release are two events because
  the kernel fades the background in between them.
- **Why the kernel's image:** the kernel has drawn Limb for its fade,
  which takes 3.6 s at 2880 x 1800 under QEMU's TCG, and computing the
  part under the panel again takes the console 1.2 s more there, after
  the release a screen of Limb with no panel. Handing the image on costs
  a message.
- **Why the report waits for the console:** the spinner says the system
  is still preparing, and the console's panel is part of what it
  prepares; with the panel prepared, the screen goes from the fade to
  the panel in a copy.
- **Why a narrowed resource:** a section holds at most
  `SECTION_MAX_SIZE` (2 MiB) and a 2560 x 1600 framebuffer is 16 MiB,
  so the console makes several; a resource covering the framebuffer is
  the authority to make them and nothing else, as a driver's resources
  cover only its function.
- **Why the console as the fallback:** it draws a whole screen from its
  own memory with no client, so a machine whose compositor fails still
  shows its owner a console to log in at, read the log and fix it.
- **Cost:** the first process could draw over the console; it already
  holds the authority over every device (ipc.md I6, Cost). Programs a
  session showed when the compositor was given up run on unseen. The
  report waits on the console, so a console that never answers keeps
  the spinner turning; one that ends closes its control channel, which
  ends the wait. The kernel's image, 19.8 MiB at 2880 x 1800, lives
  until the fade's end, and is taken while the first process starts,
  so the two take from the page allocator at once: where it runs short,
  the kernel fades in no image, or the first process's startup fails.
- **Not taken:** a section over the framebuffer that the kernel creates
  and passes in the bootstrap message, as I6 planned for a compositor.
  Its cost is a size limit no framebuffer of a modern display fits, and
  a handle the first process holds before the kernel stops drawing.
  The kernel drawing the background before it starts the first process,
  and queueing it behind the bootstrap message: its cost is the drawing's
  seconds ahead of the process's startup, where they otherwise overlap
  whatever the process waits on. Only the pixels under the panel: their
  cost is a kernel that lays out the console's panel, and an image no
  compositor can take as its wallpaper.

## C2. Starting the console

**Decided.**

The console, `\nova\console.exe`, is the input drivers' client while no
compositor runs ([input.md K3](input.md#k3-the-report-and-the-client)):
where the first process starts no compositor, the platform driver starts
it in the root job when a driver first reports, supervises it, and hands
it each endpoint the driver reports on the channel it keeps to
it. The first message on that channel is `INPUT_CLIENT_MESSAGE_SIZE` (4)
bytes, a transaction ID of 0, and three handles, in the order
`include/console_protocol.h` gives: the debug log, the console's own
process with `MAP`, so that it can map sections for its buffers and the
framebuffer, and its end of a control channel whose other end the first
process keeps.

On the control channel the first process sends three kinds of
message, each a `CONSOLE_SETUP` of 8 bytes, a transaction ID of 0 and a
kind, with the handles the kind gives:

| Kind | Bytes after the setup | Handles |
|---|---|---|
| `CONSOLE_SETUP_DISPLAY` (1) | The `BOOTSTRAP_DISPLAY` | An MMIO resource covering the framebuffer (C1) |
| `CONSOLE_SETUP_SESSION` (2) | None | The console's end of a session (C4), with `READ`, `WRITE` and `WAIT` |
| `CONSOLE_SETUP_BACKGROUND` (3) | The `BOOTSTRAP_DISPLAY` of the display it is to be given, then the kernel's `BOOTSTRAP_BACKGROUND` label | The background's sections, with `READ` alone (C5) |

The console answers the background, once its panel is prepared or the
background refused, on the same channel: `CONSOLE_PREPARED_SIZE` (4)
bytes, a transaction ID of 0, and no handles. Its end of the control
channel carries `READ`, `WRITE` and `WAIT`.

The background comes once, before the release, to a console started
before it, and the display once, after the release; a console
restarted after the release gets the display once it has started, and
no background. Until the display comes the console draws nothing, but
serves its sessions and keeps what they write, and draws it all when
it does; on a screen too narrow for 80 columns, lines written before
then are cut at its width.

A program gets a session from the first process: it creates a channel
of `CONSOLE_SESSION_CAPACITY` messages, sends one end to the console and
passes the other to the program with `CONSOLE_SESSION_RIGHTS`, which
include `TRANSFER`, so that the program can hand it on
(`console_session` in `init/console.h`). The program on the console is
login ([accounts.md](accounts.md) A6, A7), which the first process
starts after the release, with a session, and supervises. A console
that ends closes every session; when the platform driver restarts it,
the first process gives login a session on the new console, and login
takes it in place of the one that closed. While a compositor runs, the
first process starts no console, and login is the compositor's first
client instead
([display.md D13](display.md#d13-the-graphical-login)); when the first
process gives the compositor up it starts the console in direct mode,
gives it the display, and gives login a session on it, as after a
console's restart.

**Window mode.** A console that login starts as a terminal
([display.md D4](display.md#d4-the-console-in-a-window-and-as-the-fallback))
is given a display connection in place of the framebuffer's resource,
in a `CONSOLE_SETUP` of its own kind (`include/console_protocol.h`), and
makes on it one window holding its panel; it takes keys from its
window's channel rather than from an input driver's endpoint. Its
sessions, keys, panel and font are those of direct mode, the mode in
which it holds the framebuffer, except that its shell's session is in
values mode ([shell.md L7](shell.md#l7-pipelines), rule 7): the console
draws the values the shell answers as structure, tables and records
([look.md V11](look.md#v11-windows)), where in direct mode the shell
renders them as text. In a window the panel's translucency is
the window's material
([display.md D8](display.md#d8-composition)): the console writes each
pixel of a glyph, the caret or a value's rule in the ink's colour with
its coverage as its cover, and the rest of the panel with a cover of
0.

- **Why the keyboard's client:** the console is where keys are typed,
  and the platform driver already supervises the client and hands it a
  restarted driver's endpoint (input.md K4).
- **Why a control channel of its own:** the platform driver owns the
  bootstrap channel and its messages; the display and the sessions come
  from the part of the first process that starts programs on the
  console, which keeps the other end without reaching into the platform
  driver.
- **Why one program in two modes:** the console is the one program that
  shows text with no compositor, and the shell's whole interface is its
  session protocol, so one program keeps one font, one line editor and
  one set of keys, and the fallback is code that runs every day.
- **Cost:** without an input device the platform driver starts no
  client, so there is no console; two drawing paths in the console.
- **Not taken:** the console publishing itself in the namespace for
  programs to open. Its cost is a namespace that serves for the life of
  the system, which the first milestone's does not; a session handed by
  the program's creator also keeps a program's authority to its
  creator's grant (DESIGN.md, principle 7).

## C3. Keys

**Decided.**

The console reads key events (input.md K2) and maps each key code to
the character a US keyboard gives: a key's name in the evdev header is
its unshifted label, so `KEY_A` (30) gives `a` and `KEY_1` (2) gives
`1`. Shift, either one, gives the shifted character; Caps Lock, which
each press toggles, shifts the letters alone, and Shift with Caps Lock
gives them unshifted. A repeat (value 2) acts as a press. With Control,
Alt or Super held a character key gives nothing; the editing keys still
edit.

While a read waits (C4) the keys edit its line:

| Key | Effect |
|---|---|
| A printable character | Inserted at the caret, unless the line holds its most bytes |
| Backspace | Deletes the character before the caret |
| Delete | Deletes the character at the caret |
| Left, Right | Move the caret one character |
| Home, End | Move the caret to the line's start or end |
| Enter, or the keypad's | Ends the line |
| Page Up, Page Down | Scroll the panel half its height back or forward through what it has shown |

Keys typed while no read waits are kept, up to `CONSOLE_LINE_MAX`, and
edit the next read's line as though typed then. Any key but Page Up and
Page Down scrolls the panel back to its end, and so does a write.

**The secure attention key**, Ctrl+Alt+Delete with either Ctrl, either
Alt, and Delete or the keypad's
([display.md D12](display.md#d12-the-secure-attention-key-and-global-keys)),
is never given to a read: the console gives the keyboard to login's
session, which the first process marks as login's when it gives it, so
the keys that follow reach login. A pointing device's wheel scrolls the
panel three lines a detent, back or forward through what it has shown
([input.md K2](input.md#k2-key-events-and-pointer-reports)).

- **Why a US layout:** QEMU's keyboard reports positions, which evdev
  names by their US labels; another layout is a table more, and choosing
  one is a setting nothing offers.
- **Why the secure attention key goes to login:** a program can draw a
  picture of login's prompt, but no program receives the key, so a
  person who presses it first knows that what follows is login's.
- **Cost:** the console knows no other layout, no dead keys and nothing
  past ASCII; no program on the console can use Ctrl+Alt+Delete.

## C4. Sessions

**Decided.**

A session is a channel between a client and the console. The client
writes requests on its end; the console answers a read on the same
channel. A request is a `CONSOLE_REQUEST` of 16 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `transaction`: any value; a read's reply carries it back |
| 4 | 4 | `operation`: `CONSOLE_WRITE` (1) or `CONSOLE_READ` (2) |
| 8 | 4 | `length`: a write's bytes, which follow; a read's most |
| 12 | 4 | `flags`: 0, or for a read `CONSOLE_READ_NO_ECHO` (1) |

**Write.** `length` bytes follow the request, at most
`CONSOLE_WRITE_MAX` (4,096), with no handles. The console shows them at
its output position and does not reply. Each printable ASCII byte, 20h
to 7Eh, is drawn and moves the position a column; LF (0Ah) moves it to
the start of the next line, CR (0Dh) to the start of its own, and TAB
(09h) to the next column that is a multiple of 8. Any other byte is
drawn as `?`. A line wider than the panel continues on the next. While
a read with echo waits, a write goes before the line being edited,
which moves on after it.

**Read.** Nothing follows the request, and `length` is the most bytes
the line may hold, 1 to `CONSOLE_LINE_MAX` (256). The console replies
once Enter ends the line: a `CONSOLE_REPLY` of 8 bytes, the request's
`transaction` and the line's `length`, then the line, printable ASCII
without its end. With echo the line is drawn after the output so far as
it is edited, and Enter moves the output position to the next line;
with `CONSOLE_READ_NO_ECHO` nothing of it is drawn, its end included,
and the position stays where it was. The caret blinks at the editing
position while a read waits. A session has at most one read waiting;
reads from several sessions are served in the order the console takes
them from their queues, and the keys go to the first. The reply carries
the read's transaction ID, as a reply to a call does
([ipc.md I3](ipc.md#i3-synchronous-call-and-asynchronous-queues)).

Any other message ends the session: one shorter than a request, an
unknown operation, a write whose `length` is above `CONSOLE_WRITE_MAX`
or not the bytes that follow, or whose `flags` are not 0, a read with
bytes after it, a `length` out of range or another flag, any handle,
and a read while the session's earlier read still waits. The console
then closes its end without a reply. The console's end queues up to
`CONSOLE_SESSION_CAPACITY` (16) messages, so a client that writes
faster than the console draws gets `ERR_SHOULD_WAIT` and waits for
`WRITABLE` ([ipc.md I1](ipc.md#i1-channels)).

A session ends when either end closes; a client that closes its end
while a read waits drops the read. The console serves up to
`CONSOLE_SESSIONS` (8) at once and closes one sent past them.

- **Why lines, not keys:** a login prompt and a shell read lines; the
  console edits a line where it draws it, so a client neither handles
  keys nor draws its own echo, and a password read with no echo never
  reaches the screen.
- **Why no reply to a write:** the queue's bound is the flow control,
  and a reply to every write would double the messages of the common
  case.
- **Cost:** a client cannot move the output position, clear the panel
  or colour text. Keys go to whichever read waits first, and keys typed
  ahead to whichever read comes next, of any session, so sessions that
  read at once share one keyboard.
- **Not taken:** a byte stream with a terminal's escape sequences, as
  VT100 descendants take. Its cost is a parser and a state machine for
  every byte, and editing that each client does itself in a raw mode.

## C5. The panel

**Decided.**

The console draws, over the desktop background, one dark translucent
panel with rounded corners, and in it the text in the console font,
80 columns by 30 lines, fewer columns on a screen too narrow for 80,
with a caret that blinks while a read waits. It keeps the last 1,024 lines it has shown, for
Page Up. The panel's colour mixes with the background in linear light,
as the boot screen mixes ink with black, so nothing of the background
is read back from the framebuffer: the console reads Limb's pixels
under the panel from the kernel's background, or computes them with
`gfx/limb.c`, into its backdrop, which it makes once.

**The kernel's background** (C1) holds Limb as the fade leaves it on
the screen: the screen's width by height pixels, in its pixel format,
in read-only sections ([boot-protocol.md](boot-protocol.md#startup-done)).
The console takes it only where its label is `BOOTSTRAP_BACKGROUND_LIMB`,
in `PIXEL_RGB` or `PIXEL_BGR`, at the width and height of the display
the first process says it will give, which Limb and the panel's layout
take, and where the sections are the ones an image that size takes.
Then, before the release, it maps the sections to read, one after
another, makes its backdrop from the pixels under the panel, unmaps and
closes them, and renders its font's masks and its inks' tables; once
the display comes, it copies the backdrop to the screen. Otherwise it closes the sections and
logs `nova-console: background refused` with the reason, and computes
its backdrop once the display comes, as a console restarted after the
release, which gets no background, does. Both give the same pixels,
since the kernel's image is Limb exactly (`tools/check-backdrop.c`).
The display's line, `nova-console: display`, names which with
`backdrop=section` or `backdrop=computed`, and gives the preparation's
`prepare_ms` and the drawing's `draw_ms`.

On a test disk ([disk-layout.md](disk-layout.md#files)) the first
process tells the console of a display a row taller than the one it
gives, so that the background does not match it.

The font is the wordmark's strokes made a typeface
([design/font](../../design/font/README.md)): each printable ASCII glyph
is a few capsules, rings and arcs of one width, rendered once per size
into a coverage mask with the boot screen's sampling, so that drawing
text copies masks and mixes them with the panel. A cell is twice as
tall as it is wide, and its width is the screen's height divided by
100, so 16 x 32 pixels at 2560 x 1600 and 8 x 16 at 1280 x 800.

- **Why that size:** 8 x 16 is the cell text consoles have used on
  screens of 1280 x 800 pixels and less; a 2560 x 1600 screen is
  usually the same panel at twice the density, where 16 x 32 shows the
  same text at the same size.
- **Why the label decides:** an image of another background or another
  size is not the panel's Limb, and a console restarted after the
  release, a display resized
  ([display.md D18](display.md#d18-the-gpu-path)) or a background
  customized have no image that is, so computing stays the path that
  always works and the image only spares it.
- **Why the test disk's mismatch:** every boot CI makes of the test
  disk, on three processor models and under KVM, then draws its panel
  by computing, and every boot of the system disk from the image; each
  screen of both is matched against the same render of `ci/screens.c`,
  and `ci/boot.py` checks each boot's `backdrop`, with no disk or boot
  job more.
- **Cost:** the boot protocol passes no physical size, so a large
  screen of low density shows larger text than it needs. The test
  disk's console never takes the image, so only the system disk's boots
  check that path, and on the test disk the panel comes after the fade
  as long as computing takes.
- **Not taken:** a marker file of its own, on a disk of its own, that
  asks for the mismatch. Its cost is a disk image and a boot job more
  for the check the test disk already makes.

## C6. The pointer

**Decided.**

The console takes the description that opens each endpoint
([input.md K3](input.md#k3-the-report-and-the-client)) and logs it,
in the form [log.md T1](log.md#t1-lines) gives:
`nova-console: input kind=keyboard`, or `input kind=pointer` with each
absolute axis's range, such as `x=0..32767 y=0..32767`, which is the
device's and not the user's. It then takes a keyboard's key events (C3)
or a pointer's reports
([input.md K2](input.md#k2-key-events-and-pointer-reports)), and logs
what each report does, a line for each:

| Report | Line |
|---|---|
| Sets `x` or `y` | `nova-console: pointer move mode=absolute axes=xy`, `axes` naming the axes it sets, `x`, `y` or `xy` |
| Moves `dx` or `dy` | `nova-console: pointer move mode=relative axes=xy`, naming the axes it moves |
| Presses or releases a button | `nova-console: pointer press button=left`, or `release`, `button` by evdev's name from `BTN_LEFT` to `BTN_TASK`, `left`, `right`, `middle`, `side`, `extra`, `forward`, `back` and `task`; another button, `BTN_MISC` (256) or above, by its code; a keyboard's key, below it, as `key`, with no code |
| Turns the wheel | `nova-console: pointer wheel direction=up`, or `down` |

A positive `wheel` is a turn up, as QEMU sends a turn of its wheel up
(QEMU, `virtio-input-hid.c`). Each line is built whole and written with
one call, which the kernel writes in one piece. A motion of the kind the
pointer line logged last noted, absolute or relative, whatever its axes,
and a line the same as that one, are left out, so that a run of motion,
or of turns one way, is one line, naming the axes of the run's first
report. The console draws no pointer.

- **Why no position:** a position is what a click chooses, as a key's
  code is what it types, and the console logs neither. Once anything on
  the screen takes a click, a position logged beside a press would name
  the choice, and the log leaves the machine through the serial port.
  What a report does, a press or a turn, tells no more than that a key
  was pressed.
- **Why a run is one line:** the kernel writes the debug log to the
  serial port a byte at a time, waiting for each with interrupts off
  (`kernel/x86_64/serial.c`, `debug_write`), and a moving pointer ends
  each motion with a `SYN_REPORT` of its own (evdev, `EV_SYN`), so a
  line a report would hold up the console and fill the log.
- **Why no pointer drawn:** the console keeps Limb's pixels only under
  its panel (C5), and a pointer drawn outside it would need the
  background beneath, read back from the framebuffer, which the console
  never reads, or kept for the whole screen.
- **Cost:** the log shows that the pointer moved, not where.
