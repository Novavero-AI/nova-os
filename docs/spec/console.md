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

The background the kernel draws, while the boot screen's spinner
turns, is what the screen shows after the release:

1. The kernel starts the first process, draws Limb offscreen into
   memory sections and hands them to the process on its bootstrap
   channel, labelled with the background they hold and its size
   ([boot-protocol.md](boot-protocol.md#startup-done)).
2. The first process, its own startup done, takes the background and
   reports its startup done at once.
3. The kernel stops the spinner, fades the boot screen into Limb and
   releases the display.
4. The first process starts the compositor and sends it the background
   and then the display
   ([display.md D2](display.md#d2-who-holds-the-display)), as
   [display.md D19](display.md#d19-the-build-plan)'s phase 3 arrangement
   gives, rule 2.

A compositor runs on every disk but the specimen's, where the first
process closes the background's sections once it has them and gives
the display to the type specimen, as below. The console is sent no
background on any disk: in a window it draws on no screen (C2), and
with the display after a compositor it computes Limb (C5).

The kernel holds its handles to the sections until the fade's last
frame. The first process holds its own from the kernel's message
through the release, until it sends them to the compositor, and closes
them at once where the display goes to the type specimen. The
compositor holds them while they are its wallpaper
([display.md D2](display.md#d2-who-holds-the-display)). No handle
carries `WRITE`; once every holder has let go, the pages return to the
page allocator.

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
  and handing the image on costs a message, where the compositor
  computing it again costs the time
  [display.md D2](display.md#d2-who-holds-the-display) gives, with the
  screen still meanwhile.
- **Why a narrowed resource:** a section holds at most
  `SECTION_MAX_SIZE` (2 MiB) and a 2560 x 1600 framebuffer is 16 MiB,
  so the console makes several; a resource covering the framebuffer is
  the authority to make them and nothing else, as a driver's resources
  cover only its function.
- **Why the console as the fallback:** it draws a whole screen from its
  own memory with no client, so a machine whose compositor fails still
  shows its owner a console to log in at, read the log and fix it.
- **Why the console takes no background:** a compositor runs on every
  disk but the specimen's, whose console takes no display, so no console
  holds the display at the release, and one that takes it once a
  compositor is given up finds the kernel's image closed by then. A
  message that offers the console the background, its answer, a panel
  prepared from the image and a startup report that waits for that
  answer would be run by no boot and no host check. So
  `CONSOLE_SETUP_BACKGROUND` and its answer are retired (C2), the
  console has no receiver for them and makes its backdrop one way (C5),
  and the first process's report waits on no console. Its cost is a
  change of protocol; a console that computes Limb under its panel
  whenever it takes the display, 1.2 s at 2880 x 1800 under QEMU's TCG,
  before its panel shows; and `WRITE` on the console's end of the
  control channel, which its bootstrap message keeps
  ([interface.md P11](interface.md#p11-numbers-and-change), rule 2) and
  the console does not use. Not taken: keeping the message, its answer
  and the console's preparation from it; its cost is a message and code
  that nothing runs, to keep correct by reading.
- **Cost:** the first process could draw over the console; it already
  holds the authority over every device (ipc.md I6, Cost). Programs a
  session showed when the compositor was given up run on unseen. The
  kernel's image, 19.8 MiB at 2880 x 1800, lives until the fade's end,
  and is taken while the first process starts, so the two take from the
  page allocator at once: where it runs short, the kernel fades in no
  image, or the first process's startup fails.
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

On the control channel the first process sends two kinds of
message, each a `CONSOLE_SETUP` of 8 bytes, a transaction ID of 0 and a
kind, with the handles the kind gives:

| Kind | Bytes after the setup | Handles |
|---|---|---|
| `CONSOLE_SETUP_DISPLAY` (1) | The `BOOTSTRAP_DISPLAY` | An MMIO resource covering the framebuffer (C1) |
| `CONSOLE_SETUP_SESSION` (2) | None | The console's end of a session (C4), with `READ`, `WRITE` and `WAIT` |

Kind 3, `CONSOLE_SETUP_BACKGROUND`, and its answer are retired
([interface.md P11](interface.md#p11-numbers-and-change), rule 3): no
first process sends the kind, no other kind takes its number, and the
console drops a message of it as it drops one of a kind it does not
have (C1). The console answers nothing on the control channel. Its end
of the channel carries `READ`, `WRITE` and `WAIT`.

The display comes once, after the release; a console restarted after
the release gets the display once it has started. Until the display
comes the console draws nothing, but serves its sessions and keeps what
they write, and draws it all when it does; on a screen too narrow for
80 columns, lines written before then are cut at its width. A
compositor runs on every disk but the specimen's, so the console is
sent a connection to it once both have started
([display.md D19](display.md#d19-the-build-plan), rule 2, step 4), and
the display after the compositor once the compositor is given up, or,
if the console starts after that, the display.

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
gives it the display after the compositor, and gives login a session
on it, as after a console's restart. Until display.md's phase 4 builds
login's window, the console instead runs beside the compositor in a
window and keeps login's session, as
[display.md D19](display.md#d19-the-build-plan)'s phase 3 arrangement
gives.

The first process sends three more kinds on the control channel, each
with the bytes after the setup and the handles below:

| Kind | Bytes after the setup | Handles |
|---|---|---|
| `CONSOLE_SETUP_CONNECTION` (4) | None | The console's end of a display connection ([display.md D5](display.md#d5-connections-labels-and-layers)), with `READ`, `WRITE`, `WAIT` and `TRANSFER` |
| `CONSOLE_SETUP_DISPLAY_AFTER` (5) | The `BOOTSTRAP_DISPLAY` | An MMIO resource covering the framebuffer, as for `CONSOLE_SETUP_DISPLAY` |
| `CONSOLE_SETUP_OPTIONS` (6) | A `UINT32` of option bits: `CONSOLE_OPTION_REDUCE_MOTION` (0x1), from `\NOVA\NOMOTION` on a test disk ([display.md D19](display.md#d19-the-build-plan)) | None |

`CONSOLE_SETUP_DISPLAY_AFTER` is the display after another writer, a
compositor, whose last frame the screen still shows: the console draws
Limb over the whole screen, computed at the display's size, and then
its panel, where `CONSOLE_SETUP_DISPLAY` follows the kernel's release
and draws the panel alone over the kernel's Limb. A console in window
mode that gets it leaves window mode. The first process sends the
options once to each console it starts, the first and every restart, as
the first message on its control channel, before the display or the
connection. Their bits are 0 but those a test disk's flag
files set. With `CONSOLE_OPTION_REDUCE_MOTION` the caret does not blink
and stays lit ([ui.md U10](ui.md#u10-accessibility)); without it the
caret blinks (C4).
The control channel queues `CONSOLE_CONTROL_CAPACITY` messages,
`CONSOLE_SESSIONS` + 3: the options, a connection, the display or the
display after a compositor, and a session for each the console serves.

**Window mode.** A console given a display connection, by login as a
terminal
([display.md D4](display.md#d4-the-console-in-a-window-and-as-the-fallback))
or by the first process in phase 3, makes on it one window holding its
panel; it takes keys from its window's channel rather than from an
input driver's endpoint. It lays its panel out as in direct mode, for
the screen `DISPLAY_SCREEN` gives
([display.md D6](display.md#d6-windows-and-buffers)), and asks for a
window of the panel's box's size on the `PANEL` surface, which it
presents one frame at a time, as the compositor's frame event for the
last allows, into the buffer the compositor has released. When its
connection closes it does not end: it keeps its sessions and what they
write, and waits on the control channel for a new connection or for
the display after the compositor. Its
sessions, keys, panel and font are those of direct mode, the mode in
which it holds the framebuffer, except that a terminal's shell may ask
for its session in values mode ([shell.md L7](shell.md#l7-pipelines),
rule 7), once the shell has its interpreter: the console then draws
the values the shell answers as structure, tables and records
([look.md V11](look.md#v11-windows)), where otherwise the shell
renders them as text. Login's session in phase 3, and a terminal's
until the interpreter, are text. In a window the panel's translucency is
the window's material
([display.md D8](display.md#d8-composition)): the console writes each
pixel of a glyph, the caret or a value's rule in the ink's colour with
its coverage as its cover, and the rest of the panel with a cover of
0. A mask's coverage, 0 to 16 (`gfx/raster.h`), is the cover
`(coverage * 255 + 8) / 16`, rounded down. Where the caret's stroke
overlaps a glyph, the pixel is the two inks as one: its cover is
`g + c - g * c / 255` for the glyph's cover `g` and the caret's `c`,
and its colour is the caret's ink mixed over the glyph's, in linear
light as `gfx/raster.c` mixes, by `c` over that cover. One function in
`console/panel.c` makes these pixels, and the host's render calls it
too. A panel in a window need not match one drawn directly: each is
matched against the host's render of its own path.

A window's frame damages the smallest rectangle of the window that holds
every cell that differs from the frame last presented, a character or
the caret's presence, and draws only the cells that differ from what the
buffer it draws in last showed, every cell in a buffer it has drawn
nothing in; the first frame, and the first after a configure, damage
the window whole. A
configure of another size than the one it asked for, which D6 allows the
compositor to give, and a screen too small to lay the panel out on, end
the window and its connection with a refusal, logged as
[display.md D19](display.md#d19-the-build-plan), rule 8, gives; the
console then waits as it does when a connection closes. A step that
fails ends them likewise, logged as a failure.

- **Why the keyboard's client:** the console is where keys are typed,
  and the platform driver already supervises the client and hands it a
  restarted driver's endpoint (input.md K4).
- **Why a control channel of its own:** the platform driver owns the
  bootstrap channel and its messages; the display and the sessions come
  from the part of the first process that starts programs on the
  console, which keeps the other end without reaching into the platform
  driver.
- **Why the options come in the setup, first:** the first process holds
  the boot disk's files and knows whether the disk is a test disk,
  which the console does not; a restarted console is a new process that
  remembers nothing of the last, so each gets them, ahead of anything it
  draws.
- **Why a steady caret is lit:** a caret that stopped in its hidden
  half would show the reader nothing of where the line is edited.
- **Why a frame damages the cells that changed:** the caret's blink and
  a typed character change one cell, and a rectangle of the cells that
  differ spares the compositor composing, and the console drawing, the
  rest of the panel; the two buffers alternate, so each remembers what
  it shows and is drawn where it differs, not where the last frame did.
  Its cost is the cells' state kept for each buffer. Not taken: presenting
  the whole window each frame; its cost is the whole panel composed at
  every blink.
- **Why a window of another size is refused:** the console lays its panel
  out for its box and draws it there, so a window of another size is one
  it has no layout for, and drawing the panel cut by the window's edge
  would show a screen no one chose. Its cost is no console window on a
  screen where the compositor gives less than the box. Not taken: laying
  the panel out again for the size given; its cost is a second layout
  rule, which direct mode and the host's render would need too.
- **Why one program in two modes:** the console is the one program that
  shows text with no compositor, and the shell's whole interface is its
  session protocol, so one program keeps one font, one line editor and
  one set of keys, and the fallback is code that runs every day.
- **Cost:** without an input device the platform driver starts no
  client, so there is no console; two drawing paths in the console; an
  options message at every start of a console, with no bit set on the
  system disk.
- **Not taken:** the console reading the flag file itself. Its cost is
  a console that learns which disk it boots from, and the boot files
  mapped into a second program, where the first process alone holds
  them. The console publishing itself in the namespace for
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
gives them unshifted. A repeat (value 2) acts as a press, but does not
toggle Caps Lock. With Control, Alt or Super held a character key gives
nothing; the editing keys still edit. The console takes the keys of
every keyboard it reads as one keyboard's, as the compositor does
([display.md D19](display.md#d19-the-build-plan)'s phase 3 arrangement,
rule 6): a modifier is held while any keyboard holds it, and a keyboard
whose endpoint closes lets go the modifier keys it held and none that
another holds, and leaves Caps Lock as it was.

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
([input.md K2](input.md#k2-key-events-and-pointer-reports)). In a
window the keys come as `WINDOW_KEY` events with their text, the
compositor having applied the modifiers and the keymap, and edit as the
keys do here, and the compositor sends the secure attention key to no
window ([display.md D12](display.md#d12-the-secure-attention-key-and-global-keys));
the wheel comes in a `WINDOW_POINTER` event in 120ths of a detent
([display.md D10](display.md#d10-the-pointer), rule 5), and only while
the pointer is over the window or a button pressed in it is held
([display.md D11](display.md#d11-routing-input), rule 3), so a turn with
the pointer elsewhere scrolls nothing. The console adds each turn to the
part of a detent it has turned and not scrolled, scrolls three lines for
each 120 that makes, and keeps the rest.

- **Why a US layout:** QEMU's keyboard reports positions, which evdev
  names by their US labels; another layout is a table more, and choosing
  one is a setting nothing offers.
- **Why a wheel's parts are kept:** a pointing device may turn in parts
  of a detent, and dropping each part would scroll never, where a whole
  detent a time scrolls three lines as direct mode does. The part kept
  has the sign of the turn, so a turn back cancels a part forward. Its
  cost is a line that scrolls when the parts of several turns make a
  detent, not at the turn that began it. Not taken: scrolling a line for
  each 40 of 120; its cost is a scroll of other than three lines a
  detent.
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
position while a read waits, unless the options hold it still (C2). A
session has at most one read waiting; reads from several sessions are
served in the order the console takes them from their queues, and the
keys go to the first. The reply carries the read's transaction ID, as a
reply to a call does
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
is read back from the framebuffer: the console computes Limb's pixels
under the panel with `gfx/limb.c` into its backdrop, which it makes
once.

**The backdrop** is made when the display comes: the console lays its
panel out for the display's size, computes Limb under the panel, mixes
the tint into it, renders its font's masks and its inks' tables, and
copies the backdrop to the screen. The kernel's background (C1), which
holds Limb as the fade leaves it on the screen, goes to the compositor
and never to the console
([display.md D2](display.md#d2-who-holds-the-display)); what the console
computes is that image's pixels, since the kernel's image is Limb
exactly. The display's line, `nova-console: display`, says
`backdrop=computed`, and gives the preparation's `prepare_ms` and the
drawing's `draw_ms`; the line of a display after a compositor (C2) ends
with `after=compositor`.

On a test disk ([disk-layout.md](disk-layout.md#files)) the first
process tells the compositor of a display a row taller than the one it
gives, so that the kernel's background does not match it
([display.md D19](display.md#d19-the-build-plan), rule 2) and the
compositor computes Limb, as the console does.

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
- **Why the backdrop is computed:** a console takes the display only
  after a compositor, when the kernel's image is gone (C1), and a
  display resized ([display.md D18](display.md#d18-the-gpu-path)) or a
  background customized have no image that is the panel's Limb, so
  computing is the path that always works.
- **Why the test disk's mismatch:** every boot CI makes of the test
  disk, on three processor models and under KVM, then has its compositor
  compute the wallpaper, and every boot of the system disk make it from
  the image; each screen of both is matched against the same render of
  `ci/screens.c`, and `ci/boot.py` checks each boot's `wallpaper`, and
  the fallback boot's `backdrop`, with no disk or boot job more.
- **Why the line says `after=compositor`:** the compositor's last frame
  is Limb as the kernel's fade left it, so the screen cannot show
  whether the console drew the whole screen or its panel alone. The key
  shows that it took the display after a compositor, which draws the
  whole screen (`tools/check-backdrop.c` checks that drawing), and
  `ci/boot.py` requires it in the fallback boot
  ([display.md D19](display.md#d19-the-build-plan), rule 9) and in no
  other.
- **Cost:** the boot protocol passes no physical size, so a large
  screen of low density shows larger text than it needs. The test
  disk's compositor never takes the image, so only the system disk's
  boots check that path, and on the test disk the wallpaper comes after
  the fade as long as computing takes.
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
report. The console draws no pointer. A console in a window is handed no
endpoint, takes its keys, and its wheel while the pointer is over the
window (C3), from its window, and logs none of these lines; the
compositor, which is the endpoints' client, logs them
([display.md D19](display.md#d19-the-build-plan), rule 8).

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
