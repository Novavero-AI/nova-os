# Display, windows and input

The graphical system's services and protocols: who holds the display
from boot to login to the desktop, the compositor, how a program gets a
window and shows its pixels, how they are composed onto the screen,
how input reaches one window and no other, the trusted interface no
program can imitate, what replay and the log hold, the GPU path, and
the order the parts are built in. The libraries a program draws its
interface with, the rasterizer, fonts, text, widgets, layout and
themes, are [ui.md](ui.md)'s.

The two are separate documents because they bind different things.
Everything here is a protocol or a service every graphical program
meets, and its mistakes are security holes: a window another program
can read, a key that reaches the wrong process. Everything in ui.md is
a library a program may use or leave, its own pixels drawn its own
way; its mistakes cost looks and speed. What the system's own screens
look like is [look.md](look.md)'s.

It builds on [objects.md](objects.md) (handles, rights, sections),
[ipc.md](ipc.md) (channels, bulk data in sections, device memory,
driver restart), [console.md](console.md) (the display handoff and
the console that owns the screen after it), [input.md](input.md) (the
input driver), [accounts.md](accounts.md) (login and the session),
[replay.md](replay.md) and [shell.md](shell.md) (the shell's grants, its
powerbox and its settings).

Each item gives the design, why, what it costs, and the main alternative
not taken. Every item is **Decided**. Specifications are cited by the
versions in [README.md](README.md): "VIRTIO 1.4" by section, "evdev" for
the Linux input documents and "SDM" for the Intel manual's Volume 3, as
the other specifications do; "QEMU" for QEMU's source at the tag pinned
there; and the systems and papers under [Display and interface design
references](README.md#display-and-interface-design-references) by the
names given there.

## At a glance

The kernel fades the boot screen into Limb and releases the display
([console.md C1](console.md#c1-the-display-handoff)). The first process
gives the framebuffer to the compositor, a user-mode service, with the
kernel's image of Limb as its wallpaper, so its first frame changes no
pixel, and it starts showing windows. Login is its first client: a window on the
system layer that lists the accounts. After a login, the session's
programs get windows through login, each window's owner named by the
compositor in a band of the screen no program can draw in, and logout
closes them all. Pointer and keys go to one window at a time; the
secure attention key goes to login alone. If the compositor fails and
does not come back, the console takes the framebuffer, as console.md
C1 gives, and login asks for a name on it.

## D1. The parts

**Decided.**

| Part | What it is | What it holds |
|---|---|---|
| The compositor, `compose.exe` | The user-mode service that owns the display while it runs: it composes windows, routes input and draws the strip ([D8](#d8-composition)) | The framebuffer's resource, or later a channel to the GPU driver ([D18](#d18-the-gpu-path)); every input driver's endpoint; a control channel from the first process; its closure in `/store`, read only, for its fonts; the system's settings, for its theme and reserved keys; its own process with `MAP`; the debug log |
| The console, `console.exe` | The text console of [console.md](console.md), in two roles: the fallback owner of the framebuffer when no compositor runs ([D4](#d4-the-console-in-a-window-and-as-the-fallback)), and the terminal, its panel in a window | What [console.md C2](console.md#c2-starting-the-console) gives it, or a display connection in place of the framebuffer |
| Login, `login.exe` | The program that asks who is there and starts sessions and every program in them ([accounts.md A1](accounts.md#a1-the-parts)), drawing on the system layer | What [accounts.md A1](accounts.md#a1-the-parts) gives it, the sessions' job with `CREATE_JOB` ([accounts.md A7](accounts.md#a7-the-session)), and the connection that carries `SYSTEM` and `ATTENTION` ([D5](#d5-connections-labels-and-layers)) |
| The powerbox, `powerbox.exe` | The session's trusted file chooser, share picker and consent ([D16](#d16-trusted-interface-and-the-powerbox)) | A `TRUSTED` connection; the session's view, to grant from; a channel from login, on which consent is asked |
| The HUD, `hud.exe` | The session's command bar ([ui.md U11](ui.md#u11-the-hud)) | An `OVERLAY` connection; a shell in values mode, which login starts for it |
| The notification service, `notify.exe` | The session's notifications, each drawn under its sender's label ([D20](#d20-notifications)) | A `NOTIFICATION` connection; a control channel from login, on which login asks for each program's notification channel; the service's end of each such channel |
| The renderer, `uihost.exe` | One for each program that declares an interface, drawing it from values ([ui.md U6](ui.md#u6-the-renderer)) | The program's connection, and its end of the program's UI channel |
| The input drivers, `virtinp.exe` | One process for each virtio input function, keyboard or tablet ([D10](#d10-the-pointer)) | As [input.md K1](input.md#k1-the-driver) gives |
| Programs | Programs whose recipe declares the display | A connection labelled with their name, from login, or a UI channel to their renderer |

The compositor, login and the powerbox are the trusted interface: what
they draw is the system speaking. Nothing else can draw where they
draw, read what they show, or take the keys while they hold them, and
of the three only login can be what the secure attention key shows.

- **Why:** each part holds what its job needs and no more (DESIGN.md,
  principle 7). The compositor holds every pixel and every key, so it
  is kept to composing and routing, and the code that parses what a
  stranger types, login's, or what a program asks for, the powerbox's,
  runs apart from it. The design follows nitpicker's, whose clients
  draw into buffers they provide, "imported into Nitpicker via shared
  memory", and whose server "comprises only 1,500 lines of code"
  ([nitpicker](README.md#display-and-interface-design-references),
  pages 3 and 1).
- **Cost:** a key typed at a window crosses the driver, the compositor
  and the program, two messages where a monolithic window system has
  one; every graphical program depends on the compositor staying up.
- **Not taken:** a window system inside the kernel. Its cost is a
  privileged core that parses every program's drawing requests, which
  [objects.md O1](objects.md#o1-admission-to-the-core) rules out.

## D2. Who holds the display

**Decided.** From power-on to the desktop the screen has one writer at
a time:

| From | Until | Writer | How it holds the screen |
|---|---|---|---|
| nova-boot's start | `ExitBootServices` | nova-boot | The firmware's GOP framebuffer |
| The kernel's entry | The fade's last frame | The kernel | The direct map's WC mapping of the framebuffer ([boot-protocol.md](boot-protocol.md#memory-types)) |
| The release | The compositor's first frame | Nobody; Limb stays on the screen | |
| The compositor's first frame | The compositor's end | The compositor | WC sections over the framebuffer, made through the resource the first process gave it |
| The compositor given up | The next boot | The console | As [console.md C1](console.md#c1-the-display-handoff) gives |

1. The first process starts the compositor once the kernel has released
   the display, in the root job, and sends it, on its control channel,
   the `BOOTSTRAP_DISPLAY` and a resource covering exactly the
   framebuffer's pages, as [console.md C2](console.md#c2-starting-the-console)
   has it send the console, and the kernel's background, its label and
   its sections ([boot-protocol.md](boot-protocol.md#startup-done)),
   which the first process holds from the kernel's message until then.
   The compositor is then the only process holding a resource over the
   framebuffer.
2. The compositor maps the framebuffer write-combining, in sections of
   at most `SECTION_MAX_SIZE`, and maps the background's sections to
   read, one after another, as its wallpaper, where the label is the
   wallpaper it shows, Limb, at the screen's size, as the console takes
   it for its panel ([console.md C5](console.md#c5-the-panel)). It
   holds them while the wallpaper stays that, and closes them when it
   computes another: a wallpaper the settings name, or a screen of a
   new size ([D18](#d18-the-gpu-path)). Where the label does not match,
   and in a compositor started again ([D3](#d3-the-compositors-supervision-its-restart-and-what-a-crash-shows)),
   which gets no background, it computes Limb into its own memory with
   `gfx/limb.c`. Limb's pixels are a function of the screen's size
   alone, and the fade's last step is Limb exactly, since
   `gfx_cross_fade` at full weight encodes the light it decodes; so the
   compositor's first frame, Limb alone, writes the pixels the kernel
   left there, and the handoff shows nothing.
3. It starts in the attention state
   ([D12](#d12-the-secure-attention-key-and-global-keys)), draws the
   strip, serves login's connection, and login shows its screen
   ([D13](#d13-the-graphical-login)). Until phase 4 builds these, the
   compositor runs as [D19](#d19-the-build-plan)'s phase 3 arrangement
   gives.
4. The first process gives the console no display while the compositor
   runs. The platform driver hands every input driver's endpoint to the
   compositor, which is the input drivers' client in the console's place
   ([D10](#d10-the-pointer)).

- **Why one writer:** two processes writing one framebuffer show
  whichever wrote last, and a program that can write the screen can
  draw anything, login's screen included. The release already makes the
  kernel's part end at a known moment, and a resource the first process
  narrows to the framebuffer is the authority to write it and nothing
  else ([console.md C1](console.md#c1-the-display-handoff)).
- **Why the kernel's image:** the compositor never reads the
  framebuffer. WC memory is not cacheable (SDM 14.3, Table 14-2), so
  every read of it goes to the device, and the wallpaper comes from
  memory. The kernel has drawn Limb for its fade, and the image costs
  it a message to give away, where computing Limb costs 51.5 ms at
  1280 x 800 under KVM, measured ([D9](#d9-budgets)), and 3.6 s at
  2880 x 1800 under TCG, during which the screen shows the kernel's
  Limb and nothing responds. The kernel draws only what it fades in
  and never interprets a wallpaper; the label lets each receiver keep
  the computing path that a setting, a resize and a restart need.
- **Cost:** Limb's image held for the compositor's life, 3.9 MiB at
  1280 x 800 and 19.8 MiB at 2880 x 1800, and by the first process from
  the kernel's message to the compositor's start; a restarted
  compositor computes Limb, during which the screen shows the last
  frame.
- **Not taken:** the compositor computing Limb into its own memory at
  every start. Its cost is the time above at the first start, with the
  screen frozen meanwhile, for an image the kernel has drawn already.

## D3. The compositor's supervision, its restart and what a crash shows

**Decided.**

The first process supervises the compositor as it
supervises a driver ([ipc.md I10](ipc.md#i10-driver-failure-and-restart)):
one that ends is started again, unless it was restarted within the
last 60 seconds, when it is given up.

When the compositor ends:

1. The kernel closes its handles. Its framebuffer mappings and sections
   go with them, and every client's connection and window channels
   report `PEER_CLOSED`. The screen keeps the last frame it wrote, since
   nothing writes the framebuffer until the next owner does.
2. The first process starts a new compositor with a new resource over
   the framebuffer. It computes Limb, draws it and the strip, and the
   strip says that the display restarted. Like every compositor it
   starts in the attention state, so no program's window shows until
   login ends it, and a session that was locked stays locked.
3. Login, whose connection closed, gets a new one from the first
   process, as it gets a new console session after the console restarts
   ([accounts.md A10](accounts.md#a10-supervision)); it mints a new
   session connection, shows its screen again, and only then answers
   the grant channels.
4. Each session program asks for a new connection on the display grant
   channel login gave it at its start ([D5](#d5-connections-labels-and-layers)),
   makes its windows again and presents the buffers it still holds,
   since its sections are its own memory and outlive the compositor.
   Windows come back where the compositor places new ones; their
   positions and stacking order are lost.

When the compositor is given up:

5. The first process starts a console in direct mode and gives it the
   display, as [console.md C2](console.md#c2-starting-the-console)
   gives, and the platform driver makes it the input drivers' client
   ([D10](#d10-the-pointer)). Taking the display from a compositor, it
   draws Limb over the whole screen and then its panel, so nothing of
   the compositor's last frame stays.
6. The first process gives login a session on that console, as it does
   after a console's restart ([accounts.md A10](accounts.md#a10-supervision)),
   and login, whose system connection is gone and is not replaced, asks
   for a name there. A session in progress keeps its programs, but none
   can show a window again; a program that asks for a connection is
   refused.

Until phase 4, while the console runs beside the compositor, rule 9 of
[D19](#d19-the-build-plan)'s phase 3 arrangement takes the place of
rules 5 and 6.

When a client ends, its window channels close and its windows go; the
compositor composes what lay beneath. Login, which started it, reads
its exit from the process handle it holds
([accounts.md A7](accounts.md#a7-the-session)) and may say so.

- **Why reconnect through login:** a program's connection carries the
  label of the program login started, and login is the one party that
  knows which program a process is. A program that keeps its channel to
  the party that granted its endpoint, and takes a new endpoint there
  after a restart, is how the console survives its keyboard driver's
  restart ([input.md K3](input.md#k3-the-report-and-the-client)). A
  program's buffers survive the restart, since its sections are its own
  memory, and its user's work should survive with them.
- **Why the console as fallback:** the console draws a whole screen
  from its own memory with no client, and is what
  [console.md](console.md) has boot; a machine whose compositor fails
  still lets its owner log in, read the log and fix it.
- **Cost:** a restart loses every window's place, and a program that
  does not handle `PEER_CLOSED` on its connection loses its windows for
  good. A program shown when the compositor is given up runs on unseen
  until logout ends it
  ([accounts.md A7](accounts.md#a7-the-session)). Between the
  compositor's end and its successor's first frame, the screen shows a
  stale frame.
- **Not taken:** ending every graphical program with the compositor,
  as a Wayland compositor's crash usually ends its clients. Its cost is
  a session's unsaved work lost to a fault that is not the session's.

## D4. The console in a window and as the fallback

**Decided.**

The console keeps its session protocol
([console.md C4](console.md#c4-sessions)), its keys, its panel and its
font, and gains a second place to draw:

| Mode | Its display | Its keys | When |
|---|---|---|---|
| Direct | A resource over the framebuffer, as [console.md C1](console.md#c1-the-display-handoff) gives | Every input driver's endpoint, as the input drivers' client | Before the compositor exists (phases 1 and 2, [D19](#d19-the-build-plan)) and once it is given up |
| Window | A display connection, on which it makes one window holding its panel | Key events on its window channel ([D11](#d11-routing-input)) | While the compositor runs |

A console in window mode is the terminal: each terminal window is a
console process with its own sessions, started by login when the
session asks for one with a new launcher request, `LAUNCHER_TERMINAL`
([ui.md U12](ui.md#u12-the-first-programs)). Login starts it from a
store path named `terminal`, which holds the console program, so its
connection's label is `terminal` by [D5](#d5-connections-labels-and-layers)'s
rule, with no special case. The
shell runs in it with a session in values mode, as the HUD's does
([shell.md L7](shell.md#l7-pipelines), rule 7), and the console draws
the values it answers as structure, tables and records
([look.md V11](look.md#v11-windows)); in direct mode the session is
plain text.

In window mode the panel's translucency is the window's material
([D8](#d8-composition)): the console writes each pixel of a glyph, the
caret or a value's rule in the ink's colour with its coverage as its
cover, and the rest of the panel with a cover of 0, and the compositor
mixes the ink over Limb under the panel's tint, as the console mixes
them itself in direct mode.

- **Why:** the console is the one program that can show text with no
  compositor, and the shell's whole interface is its session protocol;
  keeping one program for both modes keeps one font, one line editor and
  one set of keys, and the fallback is code that runs every day.
- **Cost:** two drawing paths in the console, the window's drawing
  values as well as text; a console in direct mode knows no pointer
  beyond what [D10](#d10-the-pointer) gives it.
- **Why `terminal`:** it is the word people know for this window on
  every platform, macOS's Terminal and Windows Terminal among them.
- **Not taken:** a separate terminal program that reimplements the
  session protocol on the toolkit. Its cost is a second line editor that
  would drift from the console's, and a fallback path no one runs. The
  label `console`, the console's own store path; it needs no new store
  path, but it reads as the fallback screen.

## D5. Connections, labels and layers

**Decided.** A program draws only through a display connection: a
channel to the compositor, whose client end carries `READ`, `WRITE`,
`WAIT` and `TRANSFER`. The compositor keeps, for each connection, what
its issuer gave it, and nothing on the connection can change that:

| Field | What it is |
|---|---|
| Label | UTF-8 text of at most `DISPLAY_LABEL_MAX` (64) bytes, a longer name cut at a character's boundary: the program's name, taken by login from its store path, as the powerbox names a program in [shell.md L18](shell.md#l18-asking-for-more-the-powerbox). The compositor shows it on the frame of every `APP` window of the connection and in the strip |
| Mark | Whether the compositor draws the system's mark before the label: set for the system's own programs that show only what the system says, login, the HUD, the powerbox and the notification service, and never for a terminal, which shows what any program run in it writes, or for a program a user runs |
| Layers | Which of `APP`, `NOTIFICATION`, `OVERLAY`, `TRUSTED` and `SYSTEM` its windows may be on ([D8](#d8-composition)) |
| Rights | `MINT`: issue connections, each with layers among its own other than `SYSTEM`, with or without the mark, and with any of `MINT` and `ACTIVATE`. `ATTENTION`: receive the secure attention key ([D12](#d12-the-secure-attention-key-and-global-keys)). `CAPTURE`: make captures ([D15](#d15-capture-copy-and-paste)). `ACTIVATE`: its first window may take the focus ([D11](#d11-routing-input)) |
| Issuer | The connection it was issued from |

`SYSTEM`, `ATTENTION` and `CAPTURE` cannot be minted: the first process
gives them once, to login's connection, so one connection holds them.

Connections are issued down a chain:

| Issuer | Connection | Label | Layers | Rights |
|---|---|---|---|---|
| The first process, on the control channel | Login's | `login`, marked | `SYSTEM`, `TRUSTED`, `OVERLAY`, `NOTIFICATION`, `APP` | `MINT`, `ATTENTION`, `CAPTURE` |
| Login, at each login | The session's, which login keeps and makes no window on | The account's name | `TRUSTED`, `OVERLAY`, `NOTIFICATION`, `APP` | `MINT` |
| The session's | The powerbox's | `powerbox`, marked | `TRUSTED` | none |
| The session's | The HUD's | `hud`, marked | `OVERLAY` | none |
| The session's | The notification service's | `notify`, marked | `NOTIFICATION` | none |
| The session's | A terminal's, the console in window mode, from the store path `terminal` ([D4](#d4-the-console-in-a-window-and-as-the-fallback)) | `terminal` | `APP` | `ACTIVATE` |
| The session's, for a program the session runs whose recipe declares the display | The program's | Its name | `APP` | `ACTIVATE` |

A connection request is `DISPLAY_CONNECT` on a connection that carries
`MINT`, or `COMPOSITOR_CONNECT` on the compositor's control channel:
the label, the mark, the layers and rights asked for, and the
compositor's end of a channel the requester created. On the control
channel a closing is `COMPOSITOR_CLOSED`. A request for
more than the connection may mint fails with `ERR_ACCESS_DENIED`. A
connection makes at most `CONNECTION_WINDOWS_MAX` (64) windows, its
popups and child windows counted with them.

**Revocation.** When a connection closes, the compositor closes every
connection issued from it, and every window and capture of each, and
tells the issuer with `DISPLAY_CLOSED` and the number it gave the
connection. At logout login closes the session's connection, so every
window, connection and capture of the session goes at once, and ends
the session's programs with `task_kill` on the jobs it made for them
([accounts.md A7](accounts.md#a7-the-session)). If login ends, its own
connection closes and everything it issued goes with it.

**Reconnecting.** Each program login starts with a connection also gets
a display grant channel to login, on which it can ask for one thing: a
new connection with the same label, mark, layers and rights, less
`ACTIVATE`. Login answers only while the compositor runs, only once the
program's last connection is gone, and only after login has shown its
own screen again
([D3](#d3-the-compositors-supervision-its-restart-and-what-a-crash-shows)).
While the compositor runs, `DISPLAY_CLOSED` says when a program's last
connection has gone; when the compositor ends, the end of login's own
connection says it of every connection issued from it, since a new
compositor knows none of them. At logout login closes every grant
channel of the session.

**Activation.** `ACTIVATE` lets the connection's first window take the
focus, once, within `ACTIVATE_NS` (10 seconds) of the connection's
issue; it lapses sooner at the user's first key or click in any other
window.

The display is one of the services a program's derivation declares and
the launcher gives
([shell.md L20](shell.md#l20-the-recipe-and-the-command-line), rule
2): a program gets a connection only if its derivation declares the
display, and its label is its name, whatever it calls itself.

- **Why labels from the issuer:** what a window says about its owner is
  what the user trusts when deciding whether to type into it, so the
  owner cannot choose it. Nitpicker gives views an "unforgeable labeling
  that represents underlying policies"
  ([nitpicker](README.md#display-and-interface-design-references),
  page 4), and Qubes paints each window's frame in its domain's colour
  and starts its title with the domain's name, from outside the domain,
  so that a rogue domain cannot paint "a window pretending to belong to
  other AppVM or dom0"
  ([Qubes](README.md#display-and-interface-design-references)). The
  mark, not the label's text, tells the system's programs apart, since
  a program's author chooses its name, `terminal` included.
- **Why a tree:** a session's hold on the screen must end with the
  session, and the kernel revokes nothing: access a holder might lose
  "is given through a server that can cut it off"
  ([objects.md O10](objects.md#o10-object-lifetime)), here the
  compositor, which cuts off a whole session by closing one connection.
- **Why rights in the compositor's record:** they are rights of the
  display protocol, enforced by its server, as directory rights are the
  directory protocol's ([objects.md O8](objects.md#o8-the-namespace));
  the kernel enforces only the channel's.
- **Cost:** login is trusted to label programs truthfully and to mark
  only the system's own; a terminal carries no mark, so the strip shows
  it as it shows any program, since a prompt in it may be any program's;
  a program that wants a second label, for a
  helper process, cannot make one; a program's windows vanish at logout
  while the program runs on. A label on a frame can still be imitated:
  a program can draw inside its window a picture of another window,
  frame and label included, and "There is no label that the display
  server can apply on a window border that cannot be visually forged by
  a client" ([EWS](README.md#display-and-interface-design-references),
  7.2). What a program cannot imitate is the strip, which no window
  covers ([D16](#d16-trusted-interface-and-the-powerbox)).
- **Not taken:** a display any program may connect to by name. Its cost
  is a window system whose every client is equally trusted, with
  nothing to tell their windows apart.

## D6. Windows and buffers

**Decided.**

**The screen.** The compositor's first message on every connection is
`DISPLAY_SCREEN`: the screen's width and height in pixels and its
scale in quarters ([D7](#d7-presenting-and-pacing), rule 5). It sends
it again whenever any of them changes
([D18](#d18-the-gpu-path)). A program sizes what it draws from it; it
still learns nothing of where its windows are
([D11](#d11-routing-input), rule 6).

A window is a channel. To make one, a client creates a
channel of `WINDOW_CAPACITY` (64) messages and sends one end in a
`DISPLAY_WINDOW` request on its connection, with the size it wants,
the layer it asks for among its connection's, and its surface;
the compositor answers on the connection with a status, and serves the
window on the channel from then on. The client's end is the window: it
can pass it to another process with `TRANSFER`, and closing it closes
the window. A connection makes at most `CONNECTION_WINDOWS_MAX` (64)
windows, and a window's title, which `WINDOW_TITLE` sets, is at most
`WINDOW_TITLE_MAX` (128) bytes of UTF-8.

**Surfaces.** A window's surface is one of the theme's, named in
[look.md V4](look.md#v4-surfaces-edges-and-shadows): `WINDOW`, `RAISED`
or `PANEL`. Its material, its corners' radius and its shadow are that
surface's ([D8](#d8-composition)); an `APP` window's frame is the
compositor's whatever its surface. A client chooses only among the
theme's surfaces, so every window still looks like the system's.

**Placement.** The compositor places each new window within the area
below the strip, the whole screen while no strip is drawn. With the
screen `W` x `H` pixels, the strip `S` deep and the window `w` x `h`,
in integers rounded down: its left is `(W - w) / 2`, and its top is
`S + (H - S) * 40 / 100 - h / 2`, raised to at least `S` and lowered
to at most `H - h` where that is not below `S`. Each further `APP`
window opened while another of its connection's is shown goes one
title bar's depth ([look.md V11](look.md#v11-windows)) right of and
below the last one placed, and back to the first place when it would
cross the area's right or bottom edge. A window is at most the area's
size in each dimension and at least twice its corners' radius, rounded
up, so that no two of its corners reach the same pixel column or row;
the compositor gives a window asked to be outside those bounds the
nearest size inside them. No move or resize takes it into the strip
([D8](#d8-composition)). The 40 percent is the console's panel's
([console.md C5](console.md#c5-the-panel)), so a console in a window
stands where it stands on its own.

Two kinds of window are made on a parent's channel rather than a
connection:

1. **A popup,** a menu or a tooltip, at a place relative to its parent,
   with the input rules of [D11](#d11-routing-input).
2. **A child window,** for part of a window another process draws, as a
   renderer gives a program a canvas
   ([ui.md U6](ui.md#u6-the-renderer)). It lies within its parent,
   moves and stacks with it, and is placed and clipped by messages on
   its parent's channel, which name it by the number the compositor
   gave it; its own channel carries only its buffers and events, so
   the process that draws in it cannot move it.

A window's pixels are buffers the client allocates. A buffer of `width`
x `height` pixels, `stride` pixels apart, is `stride` x `height` x 4
bytes, rounded up to whole pages, held in
`ceil(size / SECTION_MAX_SIZE)` sections: every one `SECTION_MAX_SIZE`
(2 MiB) but the last. The client creates them with `section_create`,
maps them one after another at consecutive addresses, so that they read
as one buffer, and sends a duplicate of each, with `READ` alone, in a
`WINDOW_BUFFER` message with the buffer's size and its number, below
`WINDOW_BUFFERS_MAX` (3). The stride is at least the width; a buffer
of more sections than a message carries is refused. The compositor maps
the sections one after another in its own address space, read only.

| Screen and window | Bytes a buffer | Sections |
|---|---|---|
| 1280 x 800 | 4,096,000 | 2 |
| 2560 x 1600 | 16,384,000 | 8 |
| 2880 x 1800 | 20,736,000 | 10 |
| 7680 x 4320, the largest | 132,710,400 | 64 |

A message carries at most `CHANNEL_MAX_HANDLES` (64) handles
([ipc.md I1](ipc.md#i1-channels)), so a buffer is at most 128 MiB; a
window is at most the screen's size in each dimension, and at most
7680 x 4320.

A pixel is 32 bits, little-endian: the colour in bits 0 to 23 as sRGB
codes in `PIXEL_BGR`'s order, blue in the lowest byte, and in bits 24
to 31 its cover, the share of the pixel the window's colour holds over
its material ([D8](#d8-composition)). The compositor converts to the
screen's order, `PIXEL_RGB` or `PIXEL_BGR`, as it writes.

- **Why the client's own sections:** the pixels are the client's memory,
  written by the client alone, and the compositor's read-only mapping
  cannot change them, so a recorded client maps no memory another
  process can write, as
  [replay.md's rule on shared memory](replay.md#shared-memory) asks.
  Bulk data moves
  through sections both sides map ([ipc.md I5](ipc.md#i5-bulk-data));
  the client allocates, so its job pays for its windows' memory. EWS
  takes from each client "a read-only shared memory region containing a
  bitmap" ([EWS](README.md#display-and-interface-design-references), 5), and
  Wayland's `wl_shm_pool` "encapsulates a piece of memory shared
  between the compositor and client"
  ([Wayland](README.md#display-and-interface-design-references),
  `wayland.xml`).
- **Why a window is a channel:** a window is then a handle like a file,
  with the kernel's rules for passing it, narrowing it and noticing its
  end ([objects.md O10](objects.md#o10-object-lifetime)): a program can
  give a helper one window and nothing else, and the compositor learns
  of a client's end from `PEER_CLOSED`, with no protocol for it.
- **Why bands of sections:** `section_create` makes at most 2 MiB, the
  pages one page of the kernel's page list holds (`native_abi.h`), and
  mapped one after another the bands are one buffer to both sides, with
  no change to sections.
- **Why a least size:** a window's corners are the coverage of its own
  rounded rectangle ([D8](#d8-composition)), and a corner made once for
  every window of its radius is exact only where no other corner's arc
  reaches the same pixels, which a window at least twice its radius,
  rounded up, across and high guarantees.
- **Cost:** a buffer at 2880 x 1800 is ten handles to send and ten
  mappings on each side; a window's memory is the client's even while
  the compositor reads it. The compositor holds a channel end for each
  window and a mapping for each section of each buffer it maps: ten
  windows of two buffers at 2880 x 1800 are 200 mappings. So a
  process's handles and mappings are bounded by its job's quota alone
  ([objects.md O3](objects.md#o3-handles) and
  [O6](objects.md#o6-no-ambient-authority)), not by a fixed table, which phase 5
  needs ([D19](#d19-the-build-plan)). A window cannot be made smaller
  than twice its radius across or high: 44 pixels at the console's
  22 pixel radius at 2880 x 1800.
- **Not taken:** buffers the compositor allocates and lends to clients.
  Its cost is the compositor's memory, and its job's quota, filled by
  whichever client asks for the most windows. `section_create` raised
  past 2 MiB, with a page list of more than one page. Its cost is a
  kernel change for what bands already give, where the protocol takes
  several sections a buffer either way. A window smaller than its
  corners, which are cut to its size. Its cost is corner pixels that
  part from the coverage of the window's own rounded rectangle.

## D7. Presenting and pacing

**Decided.**

A client shows a frame with `WINDOW_PRESENT`: the buffer
and up to `WINDOW_DAMAGE_MAX` (16) rectangles, in the buffer's pixels,
that differ from the buffer it presented before; no rectangle means the
whole buffer. The compositor reads a buffer from the moment it is
presented until another is, then answers `WINDOW_RELEASED` for the one
it stopped reading. A client draws only into a released buffer.

1. **Double buffering, client side.** With two buffers a client draws
   the next frame into one while the compositor reads the other. A
   released buffer holds the frame before last, so the client repaints
   in it the damage of both frames; the display library, `lib/display`
   ([ui.md U1](ui.md#u1-the-layers)), keeps each buffer's missed damage
   for it.
2. **Double buffering, compositor side.** The compositor never composes
   on the screen. It composes each damaged rectangle in bands of
   `TILE_ROWS` (16) rows in a buffer of its own memory, then copies each
   finished band to the framebuffer ([D8](#d8-composition)); no pixel
   reaches the screen half composed.
3. **Pacing.** The compositor composes at most once each `FRAME_NS`
   (16,666,667 ns, 60 Hz), at deadlines on `clock_monotonic`, and only
   when something is damaged. After a frame shows a window's present it
   sends the window one `WINDOW_FRAME`, with the time, and a client that
   animates draws its next frame on it.
4. **Configure.** The compositor tells a window its size, scale and
   state with `WINDOW_CONFIGURE`, with a serial that rises with each;
   a present names the serial of the configure its buffer was drawn
   for, and answers it. Until then the compositor shows the old buffer,
   cut or padded with the window's material.
5. **The scale** is the screen's height over 800, rounded to the nearest
   quarter: 1 at 1280 x 800, 2 at 2560 x 1600 and 2.25 at 2880 x 1800.
   The system setting `display.scale` overrides it. A program draws at
   the screen's pixels with the scale each configure gives, so no window
   is ever stretched.

- **Why release, not a lock:** the client never waits on the compositor
  to draw, and the compositor never reads a buffer the client is
  writing, without a lock shared between them. Wayland's protocol is
  the same: "Only after receiving wl_buffer.release, the client may
  reuse the wl_buffer", and a frame callback is "a notification when it
  is a good time to start drawing a new frame"
  ([Wayland](README.md#display-and-interface-design-references),
  `wayland.xml`).
- **Why compose in bands:** blending reads what lies beneath, which the
  framebuffer must not be read for (SDM 14.3), and a band of 16 rows of
  a 2880-pixel screen is 180 KiB, against a whole-screen back buffer
  of 19.8 MiB that would hold nothing a band does not.
- **Why the scale from the height:** the boot protocol gives no physical
  size ([console.md C5](console.md#c5-the-panel)), and the console sizes
  its cells from the height too, so the console's text and the
  interface's grow alike.
- **Cost:** the GOP framebuffer gives no vertical blank to wait for, so
  a band can land while the display scans it out, and a large change can
  show torn for one refresh; the GPU path flushes whole frames
  ([D18](#d18-the-gpu-path)). Two buffers a window double its memory.
  A client that writes a buffer the compositor still reads tears its
  own window and nothing else.
- **Not taken:** composing straight onto the framebuffer. Its cost is a
  read of WC memory for every blended pixel, and frames shown half
  composed. A scale from EDID's physical size where the firmware passes
  it. Its cost is a size the boot protocol does not pass, and a second
  way to size the screen beside the console's.

## D8. Composition

**Decided.**

Translucency is over a static wallpaper, precomputed, and
there is no full-screen blur, since all drawing is on the CPU, as the
visual direction, Horizon, has it ([look.md](look.md)).

The compositor composes, bottom to top:

| Order | Layer | What | Drawn by |
|---|---|---|---|
| 1 | Wallpaper | Limb, or another background `gfx/` computes that the system's settings name | The compositor |
| 2 | `APP` | Programs' windows, their popups and children, most recently raised on top | Their programs |
| 3 | `NOTIFICATION` | Notifications ([D20](#d20-notifications)) | The notification service |
| 4 | `OVERLAY` | The HUD | The session's own programs |
| 5 | `TRUSTED` | The powerbox's dialogs | The powerbox |
| 6 | `SYSTEM` | Login's screens: the login screen, the secure screen | Login |
| 7 | Pointer | The pointer's image ([D10](#d10-the-pointer)) | The compositor |
| 8 | Strip | A band along the top of the screen, always shown, at least `STRIP_MIN` deep, a screen's height over 100, and as deep as the system's theme makes it. At its left, the wordmark, then the marks and the focused window's label ([D16](#d16-trusted-interface-and-the-powerbox)), then the session's `APP` windows by label, the focused one marked, each a button that gives its window the focus; in its centre, a search field that opens the HUD; at its right, the compositor's notices, the clock and the user ([look.md V9](look.md#v9-the-bar)) | The compositor |

No window of any layer covers the strip or lies in its band: placement,
moves and resizes keep every window below it
([D6](#d6-windows-and-buffers)), since the strip has no fill of its own
([look.md V9](look.md#v9-the-bar)) and a window beneath its type would
show between its marks. A window's shadow and corners
are the compositor's, drawn from the theme
([ui.md U9](ui.md#u9-theming-from-tokens)), and so are an `APP`
window's frame and the mark of its focus, with its label and the title
the client sets after it. A window on `NOTIFICATION`, `OVERLAY`,
`TRUSTED` or `SYSTEM` has no frame. The compositor's theme is the
system's, read from the system's settings, never a session's, and the
strip's marks and least depth are the compositor's own, not tokens; so
login's screen is drawn the same before any session, and no session can
hide or restyle what tells the system apart.

**Materials.** A window's pixel is its colour mixed over the window's
material in linear light, its cover saying how much is colour. A
material is a tint and an amount, from the theme; under a window it is
the wallpaper beneath, mixed with the tint. Whatever lies between the
window and the wallpaper is not seen through it. For one tint and
amount, each channel of the material is a function of the wallpaper's
code alone, so a material is three tables of 256 codes, computed when
the theme loads, exactly as the console mixes its panel and the fade
mixes over black (`gfx/background.c`). A pixel of cover 255 is copied;
one of cover 0 is three lookups in the material's tables; one between
is decoded, mixed and encoded as `gfx/raster.c` mixes.

**Shapes and shadows.** A window's corners and its shadow are coverage
masks from the theme's radius, made once per size, a corner's from
`outline_coverage` ([ui.md U2](ui.md#u2-the-rasterizer), rule 6). A
corner pixel mixes the window over what is composed beneath it, and a
shadow darkens what is beneath by its level, each a table per level;
both read only the compositor's band, never the framebuffer.

**Output.** Each finished band is written to the framebuffer row by
row, from the start of the 64-byte block holding the rectangle's left
edge to the end of the block holding its last column, so that the
processor writes whole write-combining buffers in bursts rather than
partial writes (SDM 14.3.1), and the frame ends with `SFENCE`, as
`gfx_flush` does.

- **Why translucency over the wallpaper only:** the wallpaper does not
  change while the session runs, so what a translucent window shows
  costs three lookups a pixel however busy the windows beneath it are,
  and nothing beneath is composed again because a window above it is
  translucent. A blur, or a mix with the windows beneath, costs a
  recomposition of everything under a translucent window whenever any
  of it changes.
- **Why a strip:** a label on a window's frame can be drawn inside
  another window, so the mark that tells the system apart must lie
  where no window can reach. EWS's display server likewise "reserves a
  band at the bottom of the display that is used to provide labeling
  feedback" ([EWS](README.md#display-and-interface-design-references), 7.2).
  The WHATWG asks a browser in full screen to ensure "that the end user
  is aware something is displayed fullscreen", "to prevent a site from
  spoofing the end user by recreating the user agent or even operating
  system environment"
  ([Fullscreen](README.md#display-and-interface-design-references), 8);
  a strip always shown leaves no full screen to spoof from.
- **Why the compositor's frames:** the label on a window and the mark
  of the focus must be true, so the program that owns the window
  cannot draw them; and every window then moves, resizes and closes
  alike. In EWS "title bar and border rendering are performed by the
  display server"
  ([EWS](README.md#display-and-interface-design-references), 7.1),
  and in Qubes by the trusted domain's decorator
  ([Qubes](README.md#display-and-interface-design-references)). A
  program that wants a different look draws it inside its window.
- **Why frames on `APP` windows only:** the HUD, the powerbox's boxes,
  notifications and login's screens sit on layers no program can draw
  on, and the strip shows the secure or the system's mark while they
  hold the focus, so a frame there would identify nothing a program
  could fake. macOS, Windows and GNOME likewise draw their system
  surfaces without window frames.
- **Why the last column:** a rectangle's right edge is the first pixel
  it does not hold, so where its last column is a block's last pixel
  the block holding the edge is the next one, and writing it is a
  burst for no pixel of the rectangle.
- **Cost:** a translucent panel over another window shows the
  wallpaper, not the window, which a user may read as the window not
  being there; a program cannot draw its own title bar; a window's
  shape is a rounded rectangle and no other; the strip's band is on
  every screen, and no window, a game's included, covers the whole
  screen; a rectangle one pixel wide is written as a whole block, 16
  pixels.
- **Not taken:** a scene of every window blended with what lies beneath
  it, as compositors with a GPU do. Its cost on the CPU is
  [D9](#d9-budgets)'s full-screen figure on every change under a
  translucent window, and a blur several times that. True full screen
  now, for a window that asks, with the strip back on any key the
  compositor reserves; its cost is a program in true full screen that
  can draw a picture of the strip, before the overlay that answers it is
  designed. No strip, the secure attention key alone marking the trusted
  interface; its cost is a login screen with nothing on it that a
  program cannot draw. Frames each program draws, the compositor drawing
  only a label; their cost is a focus mark the program draws, which can
  claim a focus it lacks, and windows that each move and close their
  own way. Frames on every window; their cost is a title bar on the
  system's own surfaces naming what the strip already proves. The
  block holding the pixel just past the last column; its cost is a block
  no pixel of the rectangle lies in, composed and written whenever the
  rectangle ends on a block's last pixel.

## D9. Budgets

**Decided.** The compositor composes damage, not frames, so its cost
follows what changes. The measurements below are the kernel's own log
of its fade, from CI run 42404427121 at commit `efd8296` on GitHub's
`ubuntu-24.04` runners at 1280 x 800, compiled by TinyCC: a fade frame
reads Limb, makes three table lookups for each pixel and writes the
whole screen write-combining, the work of a material over the
wallpaper. At that commit the line's `draw_ms` timed Limb's drawing
alone; it now times Limb's setup, its bands' allocation and the drawing
together, 87.3 ms under KVM in CI run 42448319244 at `825db58`, where a
fade frame took 2.6 ms. Phase 3's own lines replace these figures as
the baseline.

| Measured at 1280 x 800 | KVM, one boot | QEMU's TCG, ten boots on QEMU's default processor, Skylake-Client and `-cpu max` |
|---|---|---|
| Limb computed | 51.5 ms | 531 to 1,026 ms |
| A full-screen frame of lookups and WC writes | 2.4 ms | 19.7 to 38.8 ms |

| Screen | Pixels | Full frame, KVM, extrapolated | Full frame, TCG, extrapolated | Limb, KVM, extrapolated |
|---|---|---|---|---|
| 1280 x 800 | 1,024,000 | 2.4 ms (measured) | 20 to 39 ms (measured) | 52 ms (measured) |
| 2560 x 1600 | 4,096,000 | 9.6 ms | 79 to 155 ms | 206 ms |
| 2880 x 1800 | 5,184,000 | 12.2 ms | 100 to 196 ms | 261 ms |

The larger sizes are scaled by the count of pixels and not measured; a
fade frame at 2880 x 1800 under TCG has been seen to take about 150 ms,
within the scaled range. The measured path is nearly all a material's:
only the boot screen's strokes, a small share of the screen, are mixed
at partial cover, so it leaves out nearly all the decode, mix and
encode of a pixel of partial cover, and all of the second write that
composing in a band and copying it adds
([D7](#d7-presenting-and-pacing)), both of which the console's window
meets at every glyph's edge; phase 3 measures them.

The budgets, at 60 Hz, a frame of 16.7 ms:

| Change | Pixels composed at 2880 x 1800 | Budget, KVM or hardware | Under TCG |
|---|---|---|---|
| The pointer moves | Two pointer images, the 22-point arrow with its edge ([look.md V15](look.md#v15-the-pointer)) at scale 2.25: 2 x 36 x 52 | 0.1 ms | Within a frame |
| A key typed into a field | One line of text, about 1,600 x 54 | 0.5 ms | Within a frame |
| A window of 1440 x 900 dragged | Its old and new places, at most 2 x 1,296,000 | 8 ms | 10 to 20 frames a second |
| The whole screen | 5,184,000 | 16 ms, one frame | 5 to 10 frames a second |

Memory, in the compositor and the programs, at each size:

| Screen | Limb's image | One full-screen window, two buffers | The compositor's band |
|---|---|---|---|
| 1280 x 800 | 3.9 MiB | 7.8 MiB | 80 KiB |
| 2560 x 1600 | 15.6 MiB | 31.3 MiB | 160 KiB |
| 2880 x 1800 | 19.8 MiB | 39.6 MiB | 180 KiB |

Phase 3 ([D19](#d19-the-build-plan)) measures the first two rows of the
budgets at all three sizes under KVM, and at 1280 x 800 under TCG, and
logs them, as the kernel logs its fade; a budget the measurement
misses is a defect to fix or a figure to change here, said in the
change.

- **Why:** a pointer and typing within a fraction of a frame is what
  makes an interface feel immediate, and both are small changes; the
  whole screen within a frame under KVM is what the extrapolation says
  fits, with 4 ms to spare at 2880 x 1800.
- **Cost:** under TCG, the emulation CI and a Mac without hardware
  acceleration use, anything that changes much of the screen runs at a
  few frames a second; the README's 256 MiB leaves room for few
  full-screen windows at 2880 x 1800.
- **Not taken:** a budget for full-screen animation at 60 Hz under TCG.
  Its cost is designing for an emulator at the expense of the machines
  the system runs on.

## D10. The pointer

**Decided.**

1. **The device.** QEMU's `virtio-tablet-pci` replaces `qemu-xhci` and
   `usb-tablet` in the README's Run command and in `ci/boot.py`, since
   nova-os has no USB driver. It is a virtio input function like the
   keyboard (VIRTIO 1.4 5.8), and QEMU's declares the `EV_ABS` codes
   `ABS_X` and `ABS_Y`, each from 0 to `0x7FFF`; among its `EV_KEY`
   codes the buttons `BTN_LEFT`, `BTN_RIGHT`, `BTN_MIDDLE`, `BTN_SIDE`
   and `BTN_EXTRA`; and `REL_WHEEL`, which it sends as 1 or -1 a detent,
   with a `SYN_REPORT` after each update (QEMU,
   `hw/input/virtio-input-hid.c`, `include/ui/input.h`).
2. **One driver per function.** The platform driver starts
   `\nova\virtinp.exe` for every virtio input function it finds, each in
   a job of its own, where [input.md K1](input.md#k1-the-driver) starts
   one for the first.
3. **Reports.** A pointing device's update is several events closed by
   `EV_SYN` `SYN_REPORT`, which a device should buffer or drop whole
   (VIRTIO 1.4 5.8.6.2; evdev, `SYN_REPORT`). The driver of a device
   that has `EV_ABS` or `EV_REL` collects events up to each `SYN_REPORT`
   and sends them as one pointer report: the axes that changed, the
   buttons that changed, the wheel's movement, and the time. A
   keyboard's events stay one message each ([input.md
   K2](input.md#k2-key-events-and-pointer-reports)). The first message
   on every client endpoint describes the device: keyboard or pointer,
   and for a pointer each absolute axis's range, which the driver reads
   with `VIRTIO_INPUT_CFG_ABS_INFO` (VIRTIO 1.4 5.8.4).
4. **Position.** The compositor maps an absolute axis from its range
   onto the screen: x is `(value - min) * width / (max - min + 1)`, in
   pixels and 256ths of a pixel, clamped to the screen. With QEMU's
   range a pixel of a 2880-pixel screen is about 11 tablet units, so the
   position has a fraction.
5. **The wheel** is sent to windows in 120ths of a detent, as evdev's
   high-resolution wheel codes count (evdev, `REL_WHEEL_HI_RES`), so a
   device with a finer wheel needs no change of protocol; QEMU's
   `REL_WHEEL` of 1 is 120.
6. **Relative motion.** A mouse's `EV_REL` `REL_X` and `REL_Y` reach
   the client now, as the device's own deltas summed over each report
   ([input.md K2](input.md#k2-key-events-and-pointer-reports)). How far
   they move the pointer, by the setting `pointer.speed` and any
   acceleration, clamped to the screen, is the compositor's, later, as
   is the pointer lock: a window may ask to lock the pointer, receiving
   motion without a position while the pointer is hidden, and the
   compositor grants it only to the window with the focus and ends it
   when the focus leaves.
7. **The image.** The compositor draws the pointer's image last, from
   the theme's outlines at the screen's scale, the shape the window
   under it asks for with `WINDOW_CURSOR` from the theme's set. Each
   point is the theme's value times the scale, held in quarters,
   rounded once, from that value, to the nearest 256th of a pixel, a
   half going up, as `lib/outline.c` rounds a point it computes
   ([ui.md U2](ui.md#u2-the-rasterizer), rule 2). The edge's opacity,
   a percent, is that share of `OUTLINE_OPACITY_ONE` (65,536) rounded to
   the nearest: 55,706 for [look.md V15](look.md#v15-the-pointer)'s 85.
   A motion damages the image's old and new places. With a GPU the
   device draws it ([D18](#d18-the-gpu-path)).
8. **Before the compositor.** In direct mode the console is the
   pointer's client: it draws no pointer, and the wheel scrolls its
   panel three lines a detent, back or forward through what it has
   shown.
9. **Supervision.** The input drivers' client is the compositor while
   it runs and the console after it is given up. When the platform
   driver gives the compositor up it makes the console the client and
   starts the drivers again for it, without counting a restart, where
   [input.md K4](input.md#k4-supervision) would stop a driver whose
   client is given up.

- **Why per report:** an absolute position is two axes, and a pointer
  moved in two messages would be seen for a moment at a place it never
  was; evdev groups "events into packets of input data changes
  occurring at the same moment in time" with `SYN_REPORT` (evdev,
  `EV_SYN`).
- **Why a tablet first:** QEMU counts the pointer absolute when the
  device that takes its events declares absolute axes (QEMU,
  `ui/input.c`, `qemu_input_is_absolute`), as the tablet does, which is
  why the README's Run command attaches one; and an absolute position
  needs no acceleration curve.
- **Why relative motion now:** real mice report relative motion, and
  QEMU's virtio mouse does; the driver passes the deltas on as the
  device gives them, so the policy that turns them into the pointer's
  motion can come with the compositor without a change of protocol.
- **Why a point rounded once, from the theme's value:** a scale in
  quarters puts a point's exact place between 256ths, and rounding from
  the value the theme holds gives the nearest 256th to the exact
  geometry, with no point's place depending on another's. A half goes
  up because every rounding in `lib/outline.c` does, so the pointer has
  one rule. 85 percent of 65,536 is 55,705.6, and 55,706 is the nearest
  whole opacity.
- **Cost:** two message formats on one kind of endpoint; a driver per
  input function; a point at a scale between whole ones lies up to half
  a 256th of a pixel from the exact geometry, and the edge's opacity is
  0.0006 of a percentage point over 85.
- **Not taken:** the pointer's events one message each, as keys are.
  Its cost is the half-moved pointer above, and twice the messages for
  every motion. Points rounded down; its cost is a bias of half a 256th
  of a pixel, on average, toward the top left in every point. A half
  rounded away from zero; its cost is a second rule beside
  `lib/outline.c`'s, under which a half left of or above the hotspot, a
  negative one, rounds down where one right of or below it rounds up.

## D11. Routing input

**Decided.** The compositor sends each input event to one window's
channel, and to no other:

1. **The keyboard** goes to the window with the focus. A window takes
   the focus when the user clicks it or its name in the strip, or
   switches to it with the keyboard, or once by `ACTIVATE`
   ([D5](#d5-connections-labels-and-layers));
   a `SYSTEM` window takes it while shown, a `TRUSTED` window as
   [D16](#d16-trusted-interface-and-the-powerbox) gives, and the HUD
   while open. A `NOTIFICATION` window never takes it. Any other new
   window opens without the focus, stacked below the window that has
   it, and the strip says it wants attention.
2. **A key event** carries the evdev code, the value (pressed, released
   or repeated, as [input.md
   K2](input.md#k2-key-events-and-pointer-reports) gives), the modifiers
   held, and the UTF-8 text the key gives under the keymap, from the
   console's table ([console.md C3](console.md#c3-keys)) moved into a
   library both use. A game reads codes; a text field reads text.
3. **The pointer** goes to the topmost window under it, in coordinates
   relative to that window. A button pressed in a window holds the
   pointer to it until every button is released, so a drag that leaves
   the window still reaches it. A window learns when the pointer enters
   and leaves it.
4. **A popup** opens only while its parent window has the focus. While
   open it holds the pointer and the keyboard; a click outside it closes
   it, the compositor tells its window, and the click goes nowhere. It
   closes when its parent loses the focus or a `TRUSTED` or `SYSTEM`
   window is shown, never holds input from another connection's window,
   lies on the screen below the strip, and nests at most
   `POPUP_DEPTH_MAX` (8) deep.
5. **A child window** holds nothing: the pointer over it goes to it, and
   the keyboard when the user clicks it, as for any window.
6. **No global position.** A window learns positions only relative to
   itself, and only while the pointer is over it or held by it; it is
   never told where it is on the screen. A drag held to the screen's
   edge, where the pointer stops, does show a window how far it lies
   from that edge.
7. **Backlog.** The compositor never waits for a client. Input events a
   window's channel cannot take wait in the compositor, at most
   `WINDOW_BACKLOG` (64); motions are merged into the newest; past the
   limit the oldest are dropped and the window is told, with
   `WINDOW_DROPPED`, to treat its key state as unknown, as evdev's
   `SYN_DROPPED` tells a client (evdev, `EV_SYN`). Configures, releases
   and frame messages are state, not events: the compositor keeps each
   window's latest configure and frame, and the set of its buffers
   released since it last told the window, which one `WINDOW_RELEASED`
   names whole, and sends them when the channel has room, so none is
   lost. A window whose channel stays full for `WINDOW_STALL_NS`
   (2 seconds) is closed, and the log says so. Replies on a connection
   wait the same way, at most `CONNECTION_PENDING_MAX` (16), past which
   the connection is closed.

- **Why:** a key reaches the one window the user chose and nothing
  else, so no program learns what another's user types; the pointer's
  hold makes drags work without a grab that outlives the press, and a
  popup's hold follows the focus the user gave its parent. In
  nitpicker "Only the user selects the focused view by mouse click. No
  client can define the focused view"
  ([nitpicker](README.md#display-and-interface-design-references),
  page 3); Qubes passes keyboard and mouse events to a domain "only if a
  window belonging to this AppVM has focus"
  ([Qubes](README.md#display-and-interface-design-references)); and
  Wayland's clients "don't know the global position of their surfaces,
  and cannot access other clients' surfaces"
  ([Wayland](README.md#display-and-interface-design-references),
  `Protocol.md`).
- **Cost:** a program cannot see input outside its windows for any
  purpose, so a screen reader or an automation tool needs a grant from
  the user through the system ([ui.md U10](ui.md#u10-accessibility)); a
  window that wants the focus without a user action waits for one; a
  menu cannot open in a window the user has not chosen.
- **Not taken:** X11's core protocol, in which "Multiple clients can
  select input on the same window" and an event "will be reported to
  all interested clients", and `QueryKeymap` returns "the logical state
  of the keyboard" to the client that asks; X's Security extension was
  written "to prevent untrusted applications from stealing keyboard
  input" ([X11](README.md#display-and-interface-design-references)).
  Win32's, in which "Applications are permitted to inject input only
  into applications that are at an equal or lesser integrity level"
  (`SendInput`, [Windows](README.md#display-and-interface-design-references)),
  which is every program of the user's at its level. Their cost is a
  keylogger, or a program that types into another, as an ordinary
  program.

## D12. The secure attention key and global keys

**Decided.**

Some keys belong to the system and never reach a window:

| Keys | What they do | Configurable |
|---|---|---|
| Ctrl+Alt+Delete: either Ctrl, either Alt, and Delete (`KEY_DELETE`) or the keypad's Delete (`KEY_KPDOT`) | The secure attention key | No |
| Super (`KEY_LEFTMETA` or `KEY_RIGHTMETA`) tapped alone, pressed and released with no key between, and Alt+Space | Opens and closes the HUD | A system setting, `desktop.hud-keys`, which holds both |
| Alt+Tab, Alt+Shift+Tab | Moves the focus through the session's windows | A system setting |
| Super+1 to Super+9 | Gives the focus to the first to ninth window the strip names | A system setting |
| Super+W | Closes the window with the focus | A system setting |
| Super+Up, Super+Down | Maximises, or minimises, the window with the focus | A system setting |
| Print Screen | Takes a screenshot ([D15](#d15-capture-copy-and-paste)) | A system setting |

The compositor matches them on key events before routing, and sends a
window neither the press nor the release of a key that completes one; a
window that had Ctrl and Alt pressed is told they are released. Shift
or Super held with the secure attention key do not stop it being that
key. A reserved key other than the secure attention key is a setting of
the system, which only an administrator's settings handle changes
([shell.md L24](shell.md#l24-settings-and-service-commands), rule 3), and
none may be a key that gives text, or Enter, Tab or Escape.

**The secure attention key.** On it the compositor, at once and before
any other event: ends any hold of the pointer, any popup and any
pointer lock; ends every capture; hides every `APP`, `NOTIFICATION`,
`OVERLAY` and `TRUSTED` window; gives the keyboard and pointer to the
windows of the connection that carries `ATTENTION`, login's, alone;
shows the secure mark in the strip; and sets that connection's
attention flag, which it sends as `DISPLAY_ATTENTION` as soon as the
channel has room, once
however many presses come. This is the attention state. Login then
shows its secure screen: in a session, lock, log out and back, or, at
the login screen, the login screen afresh. The state lasts until login
ends it on its connection, through a restart of login, whose new
connection takes the state over, and from phase 4 every compositor
starts in it
([D3](#d3-the-compositors-supervision-its-restart-and-what-a-crash-shows);
[D19](#d19-the-build-plan)'s phase 3 arrangement, rule 3).
While it lasts, nothing a session program draws is shown and nothing
typed reaches one. The console in direct mode does the same with its
keyboard: it never gives the key to a read, and gives login's session
the keyboard.

- **Why:** a program can draw a picture of login's screen, and a person
  who types a password into it gives it away; the one thing a program
  cannot do is receive the key, so a person who presses it first knows
  that what follows is the system. Windows asks for Ctrl+Alt+Delete
  before a password so "that users are communicating through a trusted
  path when entering their passwords", against malware "that looks like
  the standard sign-in dialog box"
  ([Windows](README.md#display-and-interface-design-references)), and
  the WHATWG counts it among the "key sequences or combinations that
  cannot be intercepted for security reasons"
  ([Fullscreen](README.md#display-and-interface-design-references), 6);
  nitpicker keeps "two magic keys that are exclusively in use by
  Nitpicker and never can be used by clients"
  ([nitpicker](README.md#display-and-interface-design-references),
  page 3). The compositor acts before login answers, so a slow or
  restarting login still leaves no program on the screen; and only
  login's windows show, so no other trusted program, the powerbox
  included, can stand in for login after the key.
- **Why Ctrl+Alt+Delete:** it is the combination people know for this
  purpose, and no program here has a use for it. The keypad's Delete
  counts, so a keyboard whose Delete is on its keypad alone can press it.
- **Why Shift and Super do not stop it:** a program that cannot receive
  the combination must not receive a variant of it either, such as
  Ctrl+Alt+Shift+Delete, which a program could otherwise draw a
  prompt for and read.
- **Why Super and Alt+Space:** a tap of Super with no key between gives
  no text and completes no other combination; Alt+Space opens the HUD
  on a keyboard without Super, or under a host that keeps Super for its
  own shortcuts.
- **Why Super for the window keys:** it is the key desktops give the
  system, and with Super held a key gives no text
  ([console.md C3](console.md#c3-keys)), so the window keys take
  nothing a program types.
- **Why reserved keys in the system's settings:** a session that could
  rebind Print Screen to Enter would take a screenshot at every Enter.
- **Cost:** a program cannot use the keys reserved; a keyboard with no
  Delete key, on its keypad or off it, cannot press the secure attention
  key; changing a reserved key needs an administrator.
- **Not taken:** global shortcuts any program can register. Its cost is
  a program that registers the keys of another's shortcuts, or of
  every letter, and reads them. A second combination for keyboards
  without Delete; its cost is one more combination taken from every
  program before any such keyboard is a target. A single dedicated key
  for the secure attention key; its cost is a key a keyboard without it
  cannot press at all. Ctrl+Space for the HUD; its cost is a third
  combination taken from every program, for keyboards Alt+Space already
  serves. Super's combinations left to programs; their cost is windows
  switched, closed, maximised and minimised by the pointer and Alt+Tab
  alone.

## D13. The graphical login

**Decided.**

1. Login's screen is a `SYSTEM` window over the wallpaper, drawn with
   the toolkit ([ui.md U12](ui.md#u12-the-first-programs)): the accounts'
   names, the selected one's password field and, while no account
   exists, the first boot's form
   ([accounts.md A9](accounts.md#a9-the-first-boot)), laid out as
   [look.md V10](look.md#v10-login) gives.
2. It lists the names with a new request, `ACCOUNTS_LIST`, on a login
   channel the first process marks as the console's. The service
   answers with every account's name, in bytewise order, and nothing
   else. A login channel not so marked is refused it, with
   `ERR_ACCESS_DENIED`. Only login holds a login channel, so the
   mark separates nothing; it is there for a login from elsewhere,
   such as a network's, which must not list names.
3. The password field draws no character of what is typed, only a mark
   for each, and the line goes to the service as
   [accounts.md A5](accounts.md#a5-checking-a-password) gives, wiped
   from login's buffers once sent.
4. While a login is checked, the window shows that it is; a failure
   comes after the service's delay, and three in a row clear the field
   and the selection, as [accounts.md A5](accounts.md#a5-checking-a-password)
   ends an attempt.
5. After a login, login mints the session's connection, starts the
   HUD, the powerbox and the notification service with theirs
   ([D5](#d5-connections-labels-and-layers)), ends the attention state
   and hides its window, and the session begins; the secure attention
   key brings the window back over it
   ([D12](#d12-the-secure-attention-key-and-global-keys)). Logout
   closes the session's connection and its grant and notification
   channels.
6. Login waits on one port for every channel it serves, its connection,
   the accounts service, its relays, the session's launcher and grant
   channels, and the powerbox's and the notification service's, and
   blocks on none of them, so it can answer the secure attention key
   while a password is being checked, which takes seconds
   ([accounts.md A4](accounts.md#a4-hashing-passwords)).

[accounts.md A5](accounts.md#a5-checking-a-password) checks a name
with no record against a stand-in account, so that a reply's timing
does not tell which names exist. Listing the names tells it outright,
to whoever is at the console, where login shows them anyway; A5's
stand-in stays for every login channel that cannot list, such as a
network login a later design adds.

- **Why:** a person picks their name rather than typing it, and sees
  which accounts the machine has; the list is the console's, and the
  console is the machine's own screen.
- **Cost:** anyone at the machine sees its account names. A name is all
  that is shown: an account has no display name or picture to show
  ([accounts.md A3](accounts.md#a3-records)).
- **Not taken:** a list only after the secure attention key. Its cost
  is a key press before every login, to hide names from a person who
  is already at the machine.

## D14. What a program cannot do

**Decided.** Each of the following follows from the handles a program
holds and the compositor's rules, not from a check a program could
pass:

| A program cannot | Because |
|---|---|
| Draw outside its windows | A window's pixels come only from buffers presented on its channel ([D6](#d6-windows-and-buffers)) |
| Draw over another program's window | Placement and stacking are the compositor's; a window is raised by the user, a new window opens below the focused one, and a popup opens only from a focused window and stays above its parent only ([D11](#d11-routing-input)) |
| Draw over the trusted interface or the strip | Its connection carries `APP` alone, beneath `TRUSTED` and `SYSTEM`, and no window covers the strip ([D8](#d8-composition)) |
| Change what its windows say it is | The label and the mark are its issuer's ([D5](#d5-connections-labels-and-layers)) |
| Post a notification under another's name | The notification service draws the label login gave the channel ([D20](#d20-notifications)) |
| Keep its windows after logout | Logout closes the session's connection, and every connection, window and capture issued from it ([D5](#d5-connections-labels-and-layers)) |
| Read another program's pixels | It holds no other program's sections and no resource over the framebuffer; captures need a granted handle ([D15](#d15-capture-copy-and-paste)) |
| Read another program's input | Each event goes to one window's channel ([D11](#d11-routing-input)) |
| Read what another program copied | The clipboard is its session's, given only on a paste that follows the user's act in the window, and emptied at logout, at a lock and at the secure attention key ([D15](#d15-capture-copy-and-paste)) |
| See the secure attention key | The compositor never routes it ([D12](#d12-the-secure-attention-key-and-global-keys)) |
| Take the focus by itself | Only a user action, or `ACTIVATE` once, gives it ([D11](#d11-routing-input)) |
| Learn where its windows are | Positions are relative to the window ([D11](#d11-routing-input)), except that a drag held to the screen's edge shows how far the window lies from it |
| Make windows at all | Without a connection it has no channel to the compositor, and only a derivation that declares the display gets one ([D5](#d5-connections-labels-and-layers)) |

- **Why:** DESIGN.md, principle 8, makes what a program could and could
  not do a fact to check; for the display the facts are these.
- **Cost:** what users of other systems take for granted needs a grant
  here: a screenshot tool, a screen reader, a remote desktop, an
  automation script.
- **Not taken:** a permission kept per program, granted once and
  checked on each use. Its cost is ambient authority again: a program
  granted once reads every window, every time.

## D15. Capture, copy and paste

**Decided.**

**Capture.** A capture is a channel to the compositor that delivers
frames of the screen or of one window into two buffers the compositor
allocates, in [D6](#d6-windows-and-buffers)'s bands of sections, sent
with `READ` alone. The compositor writes a frame into a released buffer
and sends `CAPTURE_FRAME` with its damage; the holder answers
`CAPTURE_RELEASED` once it has read it. Only the connection with
`CAPTURE`, login's, makes one; a program gets one through the trusted
interface:

1. **A screenshot.** Print Screen makes login start the screenshot
   program with a capture of one frame of the screen and a slot for one
   new file in the home ([shell.md L17](shell.md#l17-rights)); the strip
   says a screenshot was taken.
2. **Sharing.** A program whose recipe declares it asks the powerbox to
   share; the powerbox shows the user a choice of the screen or one
   window, by label, and on the user's choice login makes the capture
   and the powerbox hands it over
   ([D16](#d16-trusted-interface-and-the-powerbox)).
   While a capture lives, the strip shows the label of the program
   holding it, and the user can end it there.

A capture never shows `TRUSTED` or `SYSTEM` windows, the strip, or the
pointer while it is over them: their pixels are the wallpaper in the
frames it delivers. Login makes each capture for a session, naming
that session's connection in the request, and the capture ends when
that connection closes, as at logout
([D5](#d5-connections-labels-and-layers)), or at the secure attention
key.

**Copy and paste.** The compositor holds a clipboard for each session:
for the session's connection, shared by every connection issued from
it. Login's own connection has one of its own. A clipboard holds text,
or a value in the shell's encoding ([shell.md L7](shell.md#l7-pipelines)),
carried in messages of at most `CHANNEL_MAX_BYTES` and `CLIPBOARD_MAX`
(1 MiB) in all. A window puts its selection in its session's clipboard
with `WINDOW_COPY` and asks for it with `WINDOW_PASTE`, and the
compositor honours each only within `USER_ACT_NS` (1 second) of a key
or click it delivered to that window, so data moves between programs
only on the user's act there. The compositor empties a clipboard when
its session's connection closes, at logout, and at the secure attention
key, which a lock follows, so nothing copied in one session reaches
another or outlasts a lock. A paste into a window of another connection
than the copy's is logged with both labels, never the content.

- **Why a granted capture:** what is on a screen is everything a user
  is working on, so reading it is a power the user grants for one
  purpose, through the same powerbox that grants files.
- **Why copy and paste on the user's act:** a clipboard any program can
  read at will is a channel between every pair of programs. EWS holds
  that "No communication may occur between applications through the
  display server unless we can demonstrate an authorizing user action"
  ([EWS](README.md#display-and-interface-design-references), 2.1).
  Wayland gives the clipboard to the client the user is typing in: its
  selection "is sent to a client immediately before receiving keyboard
  focus and when a new selection is set while the client has keyboard
  focus" ([Wayland](README.md#display-and-interface-design-references),
  `wayland.xml`, `wl_data_device.selection`).
- **Why a clipboard for each session:** a clipboard kept across logout
  hands what one user copied, a password among it, to the next user's
  programs; one kept across a lock hands it to whoever unlocks.
- **Cost:** a program cannot make its own screenshots, and the shell has
  no command for one or for moving the focus, which principle 3 asks
  for, until the launcher protocol gains them, its commands made by the
  generic bridge as every service's are
  ([shell.md L24](shell.md#l24-settings-and-service-commands)); a recorded
  program cannot take a capture, since its buffers are written by the
  compositor, outside the recording
  ([replay.md](replay.md#shared-memory)); a window the user is typing
  in can read the clipboard at any key, without the user pressing
  paste, as a Wayland client with the keyboard's focus can; a copy does
  not outlast a lock; a clipboard holds at most 1 MiB, and
  dragging between windows is not specified.
- **Not taken:** a paste honoured only after the paste key the
  compositor sees, as Qubes moves its clipboard between domains only on
  keys its trusted side traps
  ([Qubes](README.md#display-and-interface-design-references)). Its
  cost is a menu's Paste that needs the key as well.
- **Not taken:** frames as channel messages. Its cost is a frame of
  2880 x 1800 in 317 messages of 64 KiB.

## D16. Trusted interface and the powerbox

**Decided.**

The trusted interface is what `SYSTEM` and `TRUSTED`
windows show: login's screens on `SYSTEM`, which only login's
connection reaches, and the powerbox's dialogs on `TRUSTED`. Four things
tell them apart from a program's windows, and a program can forge none
of the first three:

1. **The strip** shows at its left the secure mark while a `SYSTEM`
   window has the focus; the system's mark and `powerbox`, then the
   asking program's label, while a powerbox dialog has it; and a
   program's label alone while one of its windows does. No program
   draws in it, and its marks are the compositor's own, not a theme's
   ([D8](#d8-composition)).
2. **The secure attention key** brings up login's windows and no
   other's ([D12](#d12-the-secure-attention-key-and-global-keys)).
3. **Nothing is over it:** no `APP`, `NOTIFICATION` or `OVERLAY` window
   lies above a `TRUSTED` one, nothing lies above `SYSTEM`, and the
   keyboard is the trusted window's while it has the focus.
4. **The rest is dimmed:** while a `SYSTEM` or `TRUSTED` window has the
   focus, the compositor draws every other window darkened by the
   system's dimming table, and while a `TRUSTED` window has it, the
   wallpaper too ([look.md V14](look.md#v14-the-system-asking) gives the
   level). EWS overlays all other windows "with a red
   alpha-blended overlay" while a trusted session's window is active,
   and marks its reserved band
   ([EWS](README.md#display-and-interface-design-references), 7.3);
   nitpicker dims views in its X-ray mode
   ([nitpicker](README.md#display-and-interface-design-references),
   page 4). Dimming alone a program can imitate: "an application can
   implement a visibly indistinguishable child window and dim it's own
   primary window using the same algorithm" (EWS, 7.2, its spelling).
   It makes the trusted window easy to see; the strip is what proves it.

The powerbox is the session's file chooser, `powerbox.exe`, started by
login with the session's view, in a job of its own. It speaks [shell.md
L18](shell.md#l18-asking-for-more-the-powerbox)'s protocol: a program
whose recipe declares a powerbox channel asks to open a file, save a
file or pick a directory, with read or write and a reason of at most 200
characters, and cannot propose a path. The powerbox shows the program's
label, the rights asked for, the reason, marked as the program's words,
and a chooser over the session's view; it grants as the shell grants and
answers with the handle. Programs on the console get their powerbox
channel to the shell, which answers the same protocol in text ([shell.md
L18](shell.md#l18-asking-for-more-the-powerbox)); programs with windows
get it to the powerbox. The same channel carries
[D15](#d15-capture-copy-and-paste)'s request to share.

The powerbox also shows the consent of [shell.md
L20](shell.md#l20-the-recipe-and-the-command-line), rule 3, in a
graphical session. Where the shell would ask whether a program from a
store path in no profile, or a script, may have what it declares, it
asks through login, its launcher, and the powerbox shows the program's
label or the script's name, its store path or file, each declaration
as a grant, and the SHA-256 the answer is kept for. Enter allows and
Escape refuses; the shell keeps the answer as L20 gives, and a refusal
fails the launch with `launch`.

1. A dialog is shown only while a window of the asking program has the
   focus, over that window; a request from a program none of whose
   windows has the focus waits until one does. A consent, for a program
   that has no window yet, is shown when the user asks for the launch.
2. It takes no key and no click until `PROMPT_GUARD_NS` (500 ms) has
   passed both since it appeared and since the last key pressed or
   repeated, so keys the user was typing elsewhere grant nothing,
   however long they run on.
3. A program has at most one request outstanding; a second is refused
   at once.

The powerbox writes a line to the log for every request: the program's
label, what it asked for and what the user granted or refused, never
the reason's text or a file's contents.

- **Why a process of its own:** [shell.md
  L18](shell.md#l18-asking-for-more-the-powerbox) quotes Plash's
  reasons: a powerbox "must be implemented as a separate, trusted
  component", with "a trusted path to the user", and the program "cannot
  specify a default pathname". The shell is that component on the
  console because it already holds the session's view; on the desktop
  the HUD would be, but it parses everything a user types and every
  command's result, and a chooser is smaller. It is not login: a flaw a
  crafted request reaches in the powerbox gains `TRUSTED`, which the
  secure attention key hides, and not `SYSTEM`.
- **Why one protocol:** a program asks for a file the same way whether
  it runs in the terminal or in a window, and the grant it gets is the
  same (DESIGN.md, principle 3).
- **Why a forged dialog gains nothing:** a program can draw a picture of
  the powerbox, and a user can type a path into it, but the program
  still holds no handle to the file: "An application might forge the
  appearance of a SaveAs dialog, but cannot forge possession of the
  necessary file system capability" (EWS, 7.3). The marks protect what
  a user types, a password; the handles protect what a program gets.
- **Why the powerbox for consent:** the HUD's and the terminal's shells
  parse everything typed, and consent is shown where no program draws;
  the powerbox already shows grants, and a flaw in it gains `TRUSTED`,
  not login's `SYSTEM`.
- **Cost:** a process for each session, and a `TRUSTED` connection login
  hands to it; a program cannot draw its own file dialog with access to
  anything it was not granted; a dialog takes no key until the user has
  paused for half a second; dimming the rest composes the whole
  screen when the trusted interface opens and again when it closes
  ([D9](#d9-budgets)).
- **Not taken:** a file dialog drawn by the program itself, from a
  library in its process. Its cost is a program that sees the user's
  whole tree to draw the chooser, which is the authority the powerbox
  exists to withhold. Consent for declared services alone; its cost is
  paths and the console a program declares given without the asking
  L20 rule 3 decides. Login drawing consent; its cost is a request a
  program's recipe shapes parsed in the process that holds `SYSTEM`.

## D17. Replay and audit

**Decided.**

1. **A client is recorded as any process**
   ([replay.md](replay.md#inputs-through-the-kernel)):
   the events, configures, releases and frame messages it reads on its
   windows' channels, and the replies on its connection, are inputs the
   kernel records. Replaying them reproduces its execution, its pixels
   included.
2. **A window's pixels are outputs,** not inputs: the client writes its
   sections alone, and the compositor maps them read only, so a client
   maps no memory another process can write, and recording needs no
   copy of a pixel, under
   [replay.md's rule on shared memory](replay.md#shared-memory).
3. **The compositor is not recordable alone:** it maps sections its
   clients write, inputs no kernel record holds, and writes device
   memory ([replay.md](replay.md#devices)). It is recorded at its message
   boundary, as a driver is, and replayed with its clients or not at
   all.
4. **A capture is not recordable,** since the compositor writes its
   buffers ([D15](#d15-capture-copy-and-paste)); the clipboard comes in
   messages, which are recorded.
5. **The log.** The compositor writes a line for each connection it
   issues (label, layers, rights), each capture made and ended (holder,
   target), each secure attention key (its time on `clock_monotonic`),
   each restart notice,
   and each window it closes for a protocol error (the label and the
   error). Login and the powerbox log their own decisions
   ([D16](#d16-trusted-interface-and-the-powerbox)). No log line holds
   a key, a character typed, a title, a label's text beyond the
   program's name, or a pixel.

- **Why:** principle 8 holds for the display as it does for files: what
  a program was shown and given is in its recording, and what the
  system granted it is in the log, readable without a replay.
- **Cost:** a session's whole screen cannot be replayed from the
  recording of one program; what a user saw is the composition of every
  client's replay.
- **Not taken:** recording the composed screen. Its cost is a video of
  everything the user saw, the most sensitive record a system could
  keep, for an audit the log and the recordings already serve.

## D18. The GPU path

**Decided.** Nothing in a program's protocol names the framebuffer, so
a GPU changes the compositor's output and nothing above it.

1. **virtio-gpu in 2D** (VIRTIO 1.4 5.7), PCI 1AF4:1050, gets a
   user-mode driver, `virtgpu.exe`, supervised as the input driver is.
   The compositor composes into sections it shares with the driver, in
   [D6](#d6-windows-and-buffers)'s bands, which the driver attaches as
   a resource's backing store with
   `VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING` and pins for DMA
   ([ipc.md I8](ipc.md#i8-dma)); each frame is a
   `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D` of each damaged rectangle and a
   `VIRTIO_GPU_CMD_RESOURCE_FLUSH` (5.7.6.2), so the host shows whole
   frames. The sections are ordinary memory, so the compositor's bands
   become a back buffer it can also read.
2. **The pointer** becomes the device's cursor, a 64 x 64 resource
   moved with `VIRTIO_GPU_CMD_MOVE_CURSOR` on the cursor queue
   (5.7.6.6, 5.7.6.10), and a motion composes nothing.
3. **The display's size can change:** the device reports it with
   `VIRTIO_GPU_EVENT_DISPLAY` (5.7.4.2), when the host's window is
   resized, and the compositor computes the wallpaper again and
   configures every window.
4. **Boot.** The device with VGA compatibility, QEMU's `virtio-vga`,
   has the linear framebuffer nova-boot and the kernel draw on; the
   driver's first `VIRTIO_GPU_CMD_SET_SCANOUT` leaves that mode
   (5.7.7). The GPU driver starts after the release, and the compositor
   moves its output to it when it reports. The driver's resources leave
   the linear framebuffer out of its region, so the compositor stays its
   one writer, and the driver cannot make a section of another memory
   type over it ([ipc.md I6](ipc.md#i6-device-memory-and-io-ports)).
   That the firmware's GOP on `virtio-vga` gives nova-boot that
   framebuffer is not checked; the driver's first boot checks it.
5. **Acceleration, later,** composes client buffers on the GPU, each
   uploaded as a texture from the sections it already has. A buffer
   that lives in GPU memory, for a program that renders on the GPU, is a
   further kind of buffer in [D6](#d6-windows-and-buffers)'s message; a
   program that draws on the CPU sends the same messages as before.

- **Why 2D first:** it moves the compositor's output to a device that
  flushes whole frames and draws the pointer, with a handful of
  commands and no shader compiler; virtio-gpu's 3D mode "requires a gpu
  with 3D support on the host" (5.7).
- **Cost:** a second output path in the compositor; DMA from the GPU
  driver, confined only where an IOMMU confines it
  ([ipc.md I8](ipc.md#i8-dma)).
- **Not taken:** a GPU protocol in programs from the start, as a client
  of a 3D compositor renders with the GPU's own API. Its cost is a
  driver stack for 3D before any window shows.

## D19. The build plan

**Decided.** Each phase ships on its own and is checked in CI by
`ci/boot.py`, which drives the pointer and keys through QMP and takes
the screen with QMP's `screendump` (QEMU, `qapi/ui.json`). A
`screendump` in its default format, PPM, is the pixels as QEMU shows
them, with no compression to vary.

**How screens are checked.** Every expected screen is computed on the
host by the same C that draws it in the guest, built for the host, as
`design/backgrounds/check-limb.sh` builds `gfx/limb.c`; `ci/screens.c`
renders each expected screen from the same inputs, the screen's size,
the account names, the text typed, and `ci/boot.py` compares the
SHA-256 of each `screendump` with it. No image is kept in the
repository. A boot under test sets `ui.reduce-motion`, so the caret
does not blink, transitions end at once and a spinner holds still
([ui.md U10](ui.md#u10-accessibility)). Until the settings service
holds the setting, the test disk carries it as `\NOVA\NOMOTION`, an
empty file, as the specimen disk carries `\NOVA\SPECIMEN`
([disk-layout.md](disk-layout.md#files)); the first process honours it
only on a test disk and passes it on in the console's and the
compositor's setup. The system disk carries no flag file, and a check
on it takes the caret in either phase of its blink. After each step, a
key typed or the pointer moved, the check takes the screen until it
matches or `SCREEN_TIMEOUT` (60 seconds) passes. On a test disk the
caret is lit, and a screen that matches must then stay as it is for
`STEADY_SECONDS` (3 seconds), which a blinking caret does not, since it
matches whenever it is lit. The KVM boot also runs
at 2560 x 1600 and at 2880 x 1800, each with
`-vga none -device VGA,edid=on,xres=W,yres=H,vgamem_mb=32` for its
width and height: 2880 x 1800's 20,736,000 bytes pass the default
16 MiB of video memory, and without `vgamem_mb` the firmware offers
800 x 600, as a boot under QEMU 11.1.3 with its bundled EDK II showed.
Each of these boots fails unless the display line names the size it
asked for, which also checks the recipe under the QEMU and EDK2
releases CI boots with.

| Phase | Ships | Needs first | CI checks |
|---|---|---|---|
| 1. The pointer | `virtio-tablet-pci` in the README and CI; one input driver per function; pointer reports ([D10](#d10-the-pointer)); the wheel scrolls the console's panel | | QMP `input-send-event` moves the tablet and turns the wheel; the console's panel shows the earlier lines, matched against the host's render |
| 2. Outlines and fonts | The outline rasterizer, Inter converted at build time, text layout ([ui.md U2 to U4](ui.md#u2-the-rasterizer)) | | The host check of the C port against its prototype, byte for byte, on a reference sheet and on the specimen at several sizes, and the text's distance from FreeType's; in the guest a test program from `/store` draws the specimen in the console's place on a disk that asks for it, and `ci/boot.py` compares its `screendump` with the prototype's render pixel for pixel |
| 3. The compositor with one surface | `compose.exe` takes the display; the console in a window, its one client; the software pointer; materials; budgets logged ([D2](#d2-who-holds-the-display) to [D9](#d9-budgets)); all as the phase 3 arrangement below gives | The interface description language and its generator ([interface.md](interface.md)); the pointer's image ([look.md V15](look.md#v15-the-pointer)) | The login prompt in the console's window, with the pointer, matched against the host's render at every size the boots run; on the system disk the compositor's display line says `wallpaper=section`, and the composer's host check shows that a frame of the wallpaper alone writes the wallpaper's pixels, so the first frame changes nothing on the screen; the fallback boot shows the console's fallback, Limb over the whole screen and the panel, matched, and logs in on it; the budget lines at each size |
| 4. The graphical login | Login's window, `ACCOUNTS_LIST`, the strip, the attention state and the secure attention key, the session's connection and logout ([D5](#d5-connections-labels-and-layers), [D12](#d12-the-secure-attention-key-and-global-keys), [D13](#d13-the-graphical-login)); a terminal at each login, the console in window mode with its shell in text, without a frame until phase 5, until the HUD can ask for one | `task_kill` on a job ([objects.md O7](objects.md#o7-jobs)), so logout ends the session's programs; the toolkit's widgets and layout login's screen uses ([ui.md U7](ui.md#u7-widgets), [U8](ui.md#u8-layout)), drawn with the system theme's defaults ([ui.md U9](ui.md#u9-theming-from-tokens)); the secure screen and the secure and system marks ([look.md](look.md#not-designed)) | The login screen with its list matched; the account created and logged into by QMP keys; the secure attention key from the session's terminal shows login's secure screen, and keys typed after it reach no session program; after logout the screen is the login screen again; the password appears nowhere in the log |
| 5. Windows | Many windows, popups and children, frames and labels, focus, stacking, damage and release, copy and paste ([D5](#d5-connections-labels-and-layers) to [D7](#d7-presenting-and-pacing), [D11](#d11-routing-input), [D15](#d15-capture-copy-and-paste)) | A process's handles and mappings bounded by its job's quota alone, not by fixed tables ([objects.md O3](objects.md#o3-handles), [D6](#d6-windows-and-buffers)); popups and menus ([look.md](look.md#not-designed)) | A test program opens two windows with known patterns; QMP drags one and clicks the other; screens matched; the program logs which window got which key, and a key typed at one never reaches the other; a popup asked for from a window without the focus is refused |
| 6. The HUD | The HUD and its key, launching, finding, settings and nvo commands ([ui.md U11](ui.md#u11-the-hud)) | The nvo interpreter and a session in values mode ([shell.md L7](shell.md#l7-pipelines), rule 7); the settings service ([shell.md L24](shell.md#l24-settings-and-service-commands)); the store's registrations, from which the launcher reads a program's declarations ([storage.md F8](storage.md#f8-the-store-volume)) | The HUD key opens it; a typed command's table matched against the host's render; a program launched from it gets the focus |
| 7. Files, Settings and the terminal | The renderer, `uihost.exe`, and the first programs drawn through it; the terminal, in values mode; the powerbox, with consent; the notification service; capture ([D15](#d15-capture-copy-and-paste), [D16](#d16-trusted-interface-and-the-powerbox), [D20](#d20-notifications), [ui.md U6](ui.md#u6-the-renderer), [ui.md U12](ui.md#u12-the-first-programs)) | The shell's commands for files and settings, built-ins and programs ([shell.md L25](shell.md#l25-built-ins)); a job for each program, with `CREATE_JOB` for login; profiles, which say which programs ask consent ([storage.md F13](storage.md#f13-profiles-system-and-garbage-collection)) | Each program's first window matched; the terminal runs `whoami` and the file commands as the system boot does; a powerbox grant by QMP keys and the log's line for it; a program from outside the profile shows consent, and Escape fails its launch; a test program's notification shows under its label; the secure attention key with a powerbox dialog open shows login's screen alone |

**The phase 3 arrangement.** Phase 3 builds the compositor before
login has a window, so its one client is the console, which keeps
login's session as it does on its own
([console.md C2](console.md#c2-starting-the-console)). Until phase 4:

1. **Where the code lives.** The compositor is `\nova\compose.exe`, an
   8.3 name as the boot disk's names are
   ([disk-layout.md](disk-layout.md#files)), built by `build.sh` from
   `compositor/`, as the console is from `console/`; it logs as
   `nova-compositor` and the platform driver names it `compositor`, as
   `virtinp.exe` logs as `nova-virtio-input`. What it computes without
   a call, composing bands, materials, corners and the pointer's image,
   is `lib/compose.c`, which
   `tools/check-compose.c` checks on the host, in CI beside
   `tools/check-masks.c`. The console's keymap moves from
   `console/keys.c` to `lib/keymap.c`, and the console's cell rule,
   `height / CELLS_PER_HEIGHT` within `FONT_WIDTH_MIN` and
   `FONT_WIDTH_MAX`, moves from `console/panel.c` to `gfx/font.h`; the
   console and the compositor both use them ([D11](#d11-routing-input),
   rule 2). The console's checks of an input endpoint's messages and
   the lines it logs for them ([console.md C6](console.md#c6-the-pointer))
   move from `console/console.c` to `lib/endpoint.c`, which
   `tools/check-endpoint.c` checks on the host, in CI beside
   `tools/check-text.c`; the compositor uses them as the console does
   (rule 8). The compositor's bootstrap, its control channel, its
   connections and its windows are one interface description,
   `interfaces/display.desc` ([interface.md](interface.md)), which gives
   every message phase 3 uses, byte by byte, and its theme's defaults;
   `include/display_protocol.h` is generated from it and tracked
   ([interface.md P16](interface.md#p16-where-the-header-lives)).
2. **Starting,** on every disk but the specimen disk, where the
   specimen takes the display as before and no compositor starts.
   Until the compositor serves the console's window (rule 4), only a
   disk that carries `\NOVA\COMPFAIL` starts it, which then exits just
   after its first frame, and the console draws every other disk as
   before. The steps:
   1. The first process reports its startup done
      ([console.md C1](console.md#c1-the-display-handoff)) without
      waiting for the console, since no background goes to the console,
      and holds the kernel's background until step 3.
   2. Once the kernel has released the display, the platform driver
      starts the compositor in the root job, on a client channel of its
      own, and hands it each input endpoint, those reported before it
      started first ([input.md K3](input.md#k3-the-report-and-the-client)).
      It then starts the console on the channel it keeps to it, and
      hands it no endpoint while the compositor runs.
   3. The first process sends the compositor `COMPOSITOR_SETUP_OPTIONS`,
      its bits 0 but those a test disk's flag files set; then
      `COMPOSITOR_SETUP_BACKGROUND`, labelled on a test disk for a
      display a row taller, as console.md C5 labels the console's, so
      test disks check the computed Limb and the system disk the
      kernel's; then `COMPOSITOR_SETUP_DISPLAY`. It waits for no answer
      between them.
   4. It sends login's session to the console, as it does now,
      whatever follows; the console has had its options since it started
      ([console.md C2](console.md#c2-starting-the-console)). It sends
      `COMPOSITOR_CONNECT` for the console: the label `console`, no
      mark, `SYSTEM` alone and no rights; and waits for `connected`, or
      for the control channel to close, when the compositor has ended
      and rule 9 follows. With `OK` it sends the console
      `CONSOLE_SETUP_CONNECTION`
      ([console.md C2](console.md#c2-starting-the-console)). With any
      other status it logs `nova-init: connection refused status=N`, and
      the console has no display until the compositor is given up.
3. **No attention state and no strip.** The compositor starts out of
   the attention state and draws no strip, so the area below the strip
   is the whole screen ([D6](#d6-windows-and-buffers)). It reserves the
   secure attention key alone: it routes it to no window and logs it
   (rule 8), since no connection carries `ATTENTION` yet. Every other
   key reaches the window with the focus.
4. **One window.** The compositor serves one window: a second, on any
   connection, it refuses with `ERR_NO_RESOURCES`; a layer outside the
   connection's with `ERR_ACCESS_DENIED`; and a surface other than
   `PANEL` with `ERR_NOT_SUPPORTED`. A size beyond the area it gives the
   window smaller (D6). The console's window is on `SYSTEM`, so it has
   no frame and holds the focus while shown
   ([D11](#d11-routing-input), rule 1), on the `PANEL` surface
   ([look.md V4](look.md#v4-surfaces-edges-and-shadows)), of its
   panel's box's size, which D6's placement puts where the panel stands
   in direct mode. Its corners' radius is five quarters of the cell
   `gfx/font.h`'s rule gives for the screen, rounded down to whole
   pixels as `panel_layout` gives, and a corner pixel's cover
   is the coverage of the window's rounded rectangle
   (`outline_rounded_rect`) that [ui.md U2](ui.md#u2-the-rasterizer),
   rule 6, gives. Its pixels are those
   [console.md C2](console.md#c2-starting-the-console) gives for a
   window.
5. **A window's first frame.** The compositor answers `DISPLAY_WINDOW`
   with `window_made` and then sends `WINDOW_CONFIGURE`: serial 1, the
   size it gives the window, the screen's scale, and
   `WINDOW_STATE_FOCUSED` where the window takes the focus. The client
   gives its buffers and presents one naming serial 1; nothing of the
   window shows before that present. A buffer whose stride is below its
   width, or whose sections do not hold `stride` x `height` x 4 bytes
   as D6 bands them, and a present of a buffer never given, close the
   window; a damage rectangle is cut to its buffer.
6. **Input.** A key event carries `lib/keymap.c`'s text. The pointer,
   its buttons and the wheel go to the window under it, the wheel in
   120ths of a detent ([D10](#d10-the-pointer), rule 5), and the
   console scrolls its panel three lines a detent as it does in direct
   mode. Relative motion moves no pointer yet (D10, rule 6).
7. **The pointer** is not drawn until the first absolute report, and
   then with its hotspot on the top left corner of the pixel the
   position rounds down to, from
   [look.md V15](look.md#v15-the-pointer)'s contours at the screen's
   scale. A motion damages the image's old and new boxes.
8. **The log,** in [log.md T1](log.md#t1-lines)'s form, each line as
   given and its keys in this order:
   - `nova-compositor: display screen=2880x1800 wallpaper=section
     prepare_ms=0.4 first_frame_ms=31.2`: `wallpaper=computed` where it
     computes Limb; `prepare_ms`, the time to map or compute the
     wallpaper; `first_frame_ms`, from its start to its first frame's
     `SFENCE`. On a test disk
     `nova-compositor: background refused reason="another size"` comes
     before it, as the console's does.
   - `nova-compositor: connection issued number=1 label=console mark=no
     layers=system rights=none`, for each connection issued (D17, rule
     5): the layers and the rights each a comma-joined list of their
     names in lower case, or `none`.
   - `nova-compositor: connection closed number=1 label=console
     reason=malformed`, and `nova-compositor: window closed
     label=console reason=malformed`, for each the compositor closes:
     `malformed` for a message its description refuses, `buffer` and
     `present` for rule 5's, `stalled` for D11's rule 7. A connection or
     window its holder closes is not logged.
   - `nova-compositor: attention at_ms=1234.5`, for each secure
     attention key, at its time on `clock_monotonic`.
   - [console.md C6](console.md#c6-the-pointer)'s input and pointer
     lines, with `nova-compositor:` for `nova-console:` and the same
     rules. A console in a window logs none, and logs
     `nova-console: window screen=2880x1800 size=1484x1116` each time it
     makes its window.
   - On a test disk, `nova-compositor: budget change=pointer frames=16
     pixels=3744 total_ms=0.9 max_ms=0.1` for frames the pointer's
     motion alone damaged, and `change=present` for frames a window's
     present damaged: `pixels` the most composed in one of them, each
     frame timed from its first band to its `SFENCE`. Each is written
     once `BUDGET_FRAMES` (16) frames of its kind have been composed
     since its last. `ci/boot.py` moves the pointer and types keys
     until both lines have come or `SCREEN_TIMEOUT` passes, and prints
     them. It fails no boot for a figure over [D9](#d9-budgets)'s
     budget, since a shared runner's time varies; such a figure is a
     defect to fix or a budget to change there.
9. **Restarts and the fallback.** The console in a window never ends
   because its connection closed (console.md C2).
   - **The console ends:** the platform driver starts it again as it
     does now, and the first process gives login a session on it, as
     [accounts.md A10](accounts.md#a10-supervision) gives, and issues a
     new `console` connection for it as in rule 2; the compositor closed
     the old connection's window when its channel closed.
   - **The compositor ends** and is started again: the platform driver
     starts every input driver again for the new compositor, without
     counting a restart (input.md K4), and the first process issues a
     new `console` connection and sends it to the console, which makes
     its window again.
   - **The compositor is given up:** the first process sends the
     console `CONSOLE_SETUP_DISPLAY_AFTER` with a new resource over the
     framebuffer; the console draws Limb over the whole screen and then
     its panel, and the platform driver starts every input driver again
     for it, without counting a restart. Login's session is the
     console's throughout, so login sees no change.

   On a test disk that carries `\NOVA\COMPFAIL`, the first process sets
   `COMPOSITOR_OPTION_EXIT_TEST` at every start, and the compositor exits
   with `COMPOSITOR_EXIT_TEST` just after the first frame that shows a
   window's present, so the console's window has been made and shown
   once before each exit, and until then just after its first frame
   (rule 2); it is restarted once and then given up
   ([D3](#d3-the-compositors-supervision-its-restart-and-what-a-crash-shows)).
   `fallback.img` is the test disk with `\NOVA\COMPFAIL` added. Its CI
   boot requires, with the counts given:
   - `nova-compositor: background refused reason="another size"` once,
     as the console's is on a test disk;
   - for each of the compositor's two starts, `nova-compositor: display`
     with `wallpaper=computed` and `nova-init: child ended
     name=compositor exit=102`, `COMPOSITOR_EXIT_TEST`;
   - `nova-init: child restarted name=compositor` once and
     `nova-init: child given up name=compositor` once;
   - for each of the three input drivers, `nova-init: child restarted`
     once with `reason="its client restarted"`, for the compositor's
     successor (input.md K4);
   - the console's `display` line with `backdrop=computed` and
     `after=compositor` (console.md C5);
   - QMP's `screendump`, taken when the first `nova-compositor: display`
     line is read, equal to Limb alone, as `ci/screens.c` draws it with
     the console's own code. The console draws nothing until the
     compositor's second end, so the dump is of the compositor's frame;
     a first frame the compositor does not write leaves the kernel's
     Limb, the same pixels, and is not seen.

   Of the lines that report a failure it accepts exactly the three
   above, each as often as it is given, and for each driver one
   `nova-init: child restarted` with `reason="its client changed"`, for
   the console, which it does not require: a driver started for the
   compositor's second start that has not yet reported when the
   compositor is given up reports to the console, is not started again
   and logs none. For no more than the machine's three input functions;
   any other line that reports a failure fails the boot. The login it
   goes on to as the test disk's boot does, typed through the console's
   keys, with each screen it matches, is the evidence that the console
   has the drivers' endpoints.
10. **What phase 4 removes:** the `console` connection, the console's
    window beside the compositor, and a secure attention key that does
    nothing. `CONSOLE_SETUP_CONNECTION`, `CONSOLE_SETUP_DISPLAY_AFTER`
    and the fallback stay, for the terminal and for a compositor given
    up.

- **Why this order:** each phase uses what the ones before it built and
  what its row needs first, and each leaves the system working: the
  console draws every screen until phase 3, and remains the fallback
  after it.
- **Why the phase 3 arrangement:** the console already draws every
  screen login needs, so the compositor arrives with a client whose
  pixels and keys are known and checked, and every boot after it still
  logs in. A console that keeps login's session through the
  compositor's restart and give-up needs no reconnecting before login
  has a connection to reconnect.
- **Why expected screens from the host:** the drawing code is integer
  arithmetic with no machine dependence (`gfx/raster.h`), so the host
  computes the guest's pixels exactly, and a check against a kept image
  would need the image regenerated by hand for every change of design.
- **Why a matched screen is held:** a blinking caret shows its lit
  phase half the time, so one match cannot tell it from a steady caret;
  a screen that stays the same for longer than a full blink can.
- **Cost:** CI's QEMU, Ubuntu 24.04's, must give the tablet QEMU 11.1's
  configuration; its version's source, 8.2.2, does when the tablet's
  `wheel-axis` property is on, its default; screens that depend on
  time need the reduced-motion setting, and each screen checked on a
  test disk takes `STEADY_SECONDS` more. Phase 3's arrangement is a
  connection the first process issues that phase 4 removes, and its
  screens are phase 2's with a pointer, so its work shows in the log,
  the fallback and the host checks more than on the screen.
- **Not taken:** comparing screens by eye, or within a tolerance. Its
  cost is regressions in a pixel or a colour that pass. Matching the
  lit phase alone; its cost is a blink that is never seen. Phases 3 and 4
  as one, the compositor arriving with login's window; its cost is the
  window protocol, the pointer, materials, the strip, login's window
  and the toolkit it draws with in one step, with no working system
  between.

## D20. Notifications

**Decided.**

1. **The service.** `notify.exe` is the session's, started by login in
   a job of its own with a `NOTIFICATION` connection, `notify`, marked,
   minted from the session's ([D5](#d5-connections-labels-and-layers)).
   It draws each notification in a window of its own on that layer, as
   [look.md V13](look.md#v13-notifications) lays it out; the window
   never takes the keyboard ([D11](#d11-routing-input)), and the
   pointer over it reaches the service.
2. **Channels.** A program whose recipe declares notifications gets,
   from login at its start, a notification channel: login asks the
   service for one on the service's control channel, naming the
   program's label, which it takes from the program's store path as
   for a display connection
   ([D5](#d5-connections-labels-and-layers)). The service keeps the
   label with the channel, and draws it on every notification that
   comes on the channel.
3. **The request.** `NOTIFY_POST` carries a title of at most
   `NOTIFY_TITLE_MAX` (128) bytes, a body of at most `NOTIFY_BODY_MAX`
   (512) bytes, both UTF-8, and up to `NOTIFY_ACTIONS_MAX` (2) actions,
   each a label of at most `NOTIFY_ACTION_MAX` (32) bytes; a post past a
   limit is refused with `ERR_INVALID_ARGS`. The service answers
   `NOTIFY_SHOWN` with the notification's number once it is shown, then
   `NOTIFY_ACTION` with the number and the action's index when the user
   activates one, or `NOTIFY_GONE` with the number when it leaves
   without one.
4. **Limits.** Notifications are shown one at a time, in the order they
   come. A channel has at most `NOTIFY_PENDING_MAX` (4) notifications
   shown or waiting, and a post past it is refused with
   `ERR_NO_RESOURCES`, so one program cannot hold the corner.
5. **Ends.** When a channel closes its notifications go. At logout
   login closes every notification channel of the session.
6. **The log.** The service writes a line for each notification: the
   label and the action taken, never the title, the body or an
   action's text.

- **Why a service of its own:** the compositor holds every pixel and
  every key, so it is kept to composing and routing
  ([D1](#d1-the-parts)); text and actions a program supplies are parsed
  in a process that holds a `NOTIFICATION` connection and nothing else.
- **Why the channel's label:** what a notification says about its
  sender is what the user trusts, as for a window
  ([D5](#d5-connections-labels-and-layers)), so the sender cannot
  choose it.
- **Cost:** a process for each session; a program notifies only if its
  recipe declares it; a notification carries text and actions, no
  image.
- **Not taken:** the compositor drawing notifications. Its cost is
  programs' text and actions laid out in the process that holds every
  pixel and every key, which D1 keeps to composing and routing.

## Prior art

| Source | Taken | Left |
|---|---|---|
| nitpicker | A small server; buffers the client provides; focus chosen only by the user; keys reserved to the server; labels the client cannot set; the rest dimmed | X-ray mode toggled by the user, in place of a strip always shown |
| EWS | Windows made through a session capability; read-only client bitmaps; frames drawn by the server; a band of the screen reserved for labels; other windows overlaid while a trusted session's is active; a forged dialog holds no capability | |
| Qubes | Frames and title prefixes drawn outside the program; input only to the domain whose window has the focus | Colour as the only mark; a clipboard moved only by keys the trusted side traps |
| Wayland | Shared memory the client provides; damage in the buffer's coordinates; release and frame callbacks; input in surface coordinates; the clipboard given to the client with the keyboard's focus | One socket every client connects to alike, with how far to trust each left to the compositor; buffers passed as file descriptors |
| X11 | | Input any client can select on any window; the keyboard's state to any client that asks |
| Windows | Ctrl+Alt+Delete before a password, as a trusted path | Input injection into every program at the same integrity level |
| Fullscreen | Spoofing in full screen named as the risk | An overlay as the only defence |
| Plan 9's rio | A window system as an ordinary file server, each window with its own `cons`, `mouse` and `cursor` | Drawing it does not mediate, "rio places windows but does not mediate programs' access to the display device", and every window's image readable as a file |
| Scenic | Composition as a service; input "in their view's coordinate system"; no view sees "the graphical content of any other View" | A GPU scene graph from the start |
