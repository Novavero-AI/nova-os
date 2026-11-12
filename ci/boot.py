"""Boot a nova-os disk under QEMU and check what it reports.

Usage: python3 -I ci/boot.py --code CODE.fd --vars VARS.fd --log LOG
                             [--timeout SECONDS] [--expect LINE ...]
                             [--tests] [--data DATA [--corrupt-gpt KIND]]
                             [--size WIDTHxHEIGHT]
                             [--fonts FONTS [--screen SCREEN.ppm]]
                             {system,fallback,noanswer,window,persist,chain,
                              specimen,logout} DISK
                             [-- QEMU_ARG ...]

Boots DISK with the machine and devices of README.md's Run section,
headless, with the firmware CODE.fd and a copy of VARS.fd, a writable
copy of DATA as the data disk after it, and the QEMU arguments given
after --. The guest's serial port is echoed with the time since QEMU
started and written to LOG.

Every line nova-os writes has the form docs/spec/log.md T1 gives,
component: event key=value ..., and the boot is checked by the lines
it waits for, each matched by its component, its event and the keys
the check names, never by the rest of its text. With --tests, DISK is
a test disk (docs/spec/disk-layout.md), and the boot must also write a
whole test report in KTAP version 1 (log.md T2): the check prints the
report's plan, each test's result, and every test not ok with its
diagnostics, and fails on a missing version line or plan, a result out
of order, a count short of its plan, any test not ok, or a test the
boot must run missing or skipped: the kernel's self-tests, the first
process's checks of its calls, the test program from /store and the
boot disk's read, the data disk's check with a data disk whose GPT is
whole, and in chain mode the chain, each of its steps and each pinned
link. Without --tests, a report anywhere in the log fails the boot, as
does a budget line of the compositor, which only a test disk's writes,
and the kernel must report its random number generator's self-test
passing. chain, fallback, noanswer, window and specimen take only a test
disk, and fallback, noanswer and persist a whole data disk.

In system mode, the boot must report the store service serving, startup
done, the background drawn, the compositor's display, its wallpaper
made from the kernel's background or, on a test disk, whose compositor
is told of a display the background does not match, computed
(docs/spec/display.md D2), the console's connection issued and its
window made on it, and the compositor taking the description of each of
the keyboard, the mouse and the tablet, the tablet's with its axes'
range. Then, sent through QMP, QEMU's monitor protocol, the tablet
moves, each of three buttons is clicked, the wheel turns up and down
and the mouse moves, and the compositor must report each; no position
sent may appear in a line of the compositor, the console or an input
driver, and the console, in a window, must log no line for an input.
With a data disk whose GPT is whole, the accounts service must
report its self-test passing and no account, and login must start,
and QMP's screendump must come to equal, within SCREEN_TIMEOUT, the
screen ci/screens.c draws for the console's window showing login's
prompt, with the pointer at the pixel the tablet's last place gives;
then, typed through QMP, the first account is created, the shell
starts, and the shell must report running whoami for it; the password,
typed at the shell's prompt too, must appear nowhere in the log. The
session then makes a directory in its home, writes a file in it twice,
shows it, moves it, writes and removes an empty file, and is refused a
directory under /store and a file in another user's home. The shell
must log each command's status, and then the data disk, read on the
host as docs/spec/pool-format.md lays it out, must hold the account's
home alone in /users and the command's change in the home, and nothing
else there; no word of a name or line typed for the commands may
appear in the log. In system mode the session then writes a file of
one-character lines, which fill the console's panel past a screen, the
tablet moves into the console's window, and once the screen shows the
shell's prompt after the lines the wheel turns back and forward through
QMP. Before the turns and after each, QMP's screendump must come to
equal, within SCREEN_TIMEOUT, the screen ci/screens.c draws from the
same lines, prompt and turns, with the pointer where the tablet is and
the caret in either phase of its blink, built with the host's C
compiler, CC if set. The secure attention key, sent through QMP, must be
logged by the compositor and leave the screen as it was. On a test disk, whose first process sets ui.reduce-motion
for its flag file NOMOTION (docs/spec/display.md D19), the caret holds
steady: each screen must show it lit, and the screen must stay the same
for STEADY_SECONDS more. On a test disk, whose first process also has
the compositor log what its frames cost for its flag file BUDGETS
(display.md D19, rule 8), the boot then moves the tablet between two
places and types a letter and Backspace in turn at the shell's prompt,
until the compositor has logged the budget line of each kind twice or
SCREEN_TIMEOUT passes, and prints the second of each with the screen's
size (check_budgets); a figure over display.md D9's budget fails no
boot. A budget line that is not in rule 8's form fails any boot. In
persist mode the same copy of the data disk
is booted twice: the first boot is system mode's but for the wheel, and the
second must report the account the first made and, typed again, log in
with it, run whoami and show the file, which the disk must still hold.
Each session ends with logout, and login must report it ended.
In chain mode, the boot of chain.img must report the chain's steps
passing in its test report. In fallback mode, the boot of fallback.img,
a test disk whose first process makes the compositor end just after the
first frame that shows the console's window, at every start, must show
the compositor take the display and give it up (docs/spec/display.md
D19, rule 9): the compositor's refusal of the background, its display
line, which says its wallpaper is computed, the console's connection
issued and its window made, and the compositor's end with
COMPOSITOR_EXIT_TEST, each of the last three once for each of its two
starts, the console's window made before each end; the first process
restarting it once and giving it up; each of the three input drivers
started again for the compositor's second start; and the console
drawing the display after it, Limb over the whole screen and then its
panel. When the first display line is read, QMP's screendump must be
either Limb alone or the console's window over Limb, as ci/screens.c
draws them, polled until it is one (check_limb): a whole frame of the
compositor, not the order of the two. A first frame the compositor does
not write leaves the kernel's Limb, which is the same pixels, and is not
seen. Of the lines that report a failure the boot accepts only those of
the first process and the drivers, each as often as it is reported and
no more, and any other fails it; it then goes on as system mode does,
matching the screen against the host's render and logging in on the
console. In noanswer mode, the boot of noanswer.img, a test disk whose
first process has the compositor answer no connection, at every start,
must show the first process end the compositor at the deadline of its
wait for the answer, and give it up (docs/spec/display.md D3 and D19,
rules 2 and 9): the compositor's refusal of the background and its
display line, its line for each input endpoint, which shows it serving
while it does not answer, the first process's line for the wait that
failed with ERR_TIMED_OUT, and the compositor's end with
PROCESS_EXIT_KILLED, the display line, the wait's and the end each once
for each of its two starts; the first process restarting it once and
giving it up; each of the three input drivers started again for the
compositor's second start; and the console drawing the display after it.
A line for a connection issued or for the console's window fails the
boot, since the compositor answers none. Of the lines that report a
failure it accepts those the fallback boot accepts and the wait's, once
for each start, and it then goes on as the fallback boot does. In window
mode, the boot of window.img, a
test disk whose first process starts a test client that makes a window
on the compositor in the console's place (docs/spec/display.md D19,
rules 2, 4, 5, 8 and 11), must report the compositor refusing the background and computing
Limb, issuing the client's connection, and the client's window made,
its first buffer shown, and its second shown with the first released;
and then, within SCREEN_TIMEOUT of the client's report of the second,
QMP's screendump must equal the screen ci/screens.c draws for the
client's window, built with the host's C compiler, CC if set. That
screen is made by the same library code the compositor composes with,
so it shows that the compositor reads, places and composes what the
client drew as the specification says, not that the library's rules
are right, which tools/check-compose.c and tools/check-window.c check.
Then, sent through QMP, the tablet is placed away from the window and
moved onto it and about it, each button is clicked, the wheel turns up
and down, a button is pressed and the pointer dragged out of the window
before it is released, a button is pressed away from the window and
released over it, and the mouse moves. The client must log, in order and as display.md D11 gives
them, its entry, its pointer events and its leave, at the places
display.md D10 and D6 give for the values sent, and the compositor must
log each of the pointer's lines. The screen must come to equal, within
SCREEN_TIMEOUT, the one ci/screens.c draws with the pointer drawn at the
pixel D10 gives for the tablet's value, after the tablet is placed, after
it enters the window, and after the mouse's relative motion, which moves
no pointer, where it must also stay so for STEADY_SECONDS. Then keys are
typed through QMP, and the client must be sent them as display.md D11 and
D12 give. The client holds its window and its connection for the whole
boot, so a line that says the compositor closed either fails the boot.
No console connects, draws or takes a key, and no account is made. In specimen
mode, the boot of specimen.img
must report the display given to the type specimen from /store and the
specimen drawn; then QMP's screendump, kept at SCREEN.ppm if given,
must equal pixel for pixel the screen design/text/specimen.py draws at
its size with the nova-os fonts in FONTS. In logout mode, the boot of
logout.img, which takes a data disk whose GPT is whole and no --tests,
creates the account as system mode does, and the session's program, in
the shell's place, runs programs that end at once, more of them than
login has handles, and then programs that stay running until login
refuses one, at its limit on a session's programs; it ends without
waiting for them. Login must report the session ended with each of the
running ones stopped, and none may report again for QUIET_SECONDS. A
second session, logged into with the account the first made, must then
do the same, its programs refused at the same count as the first's,
which they are not if login kept any of the first's places.
In every mode but chain, the kernel must also report the frames the boot screen's spinner started
while the background was prepared, from the start of the kernel's
drawing to the first process's startup report: at least one, unless
the preparation ended within the longest a turning spinner goes
between frames.
With --size, QEMU's display is a VGA device whose EDID offers that size
in pixels, with the video memory a frame that large needs, and the
display line the console or the compositor logs must name the size, or
the boot fails at once, which also checks the recipe against the QEMU
and EDK2 releases the boot runs with.
Each --expect LINE, in T1's form, adds a line the boot must report,
with the keys LINE gives. With a data disk, the boot must also report
the file system service opening its pool and /data and /users
published. With --corrupt-gpt, the copy's two GPT tables are changed
first: header or entries change a byte of each header's CRC or of each
entry array; extent moves the pool's entry to start at LBA 0, below the
first usable LBA; usable moves the entry and the first usable LBA both
to LBA 1, the header's; overlap adds a second entry over the pool's
blocks; the last three with every CRC made to match. The boot must then
report the service refusing the disk for that check, /data and /users
not published, and the accounts service and login not started.

The check fails at once on a line from nova-os that reports a failure,
as T1's table gives them, or if QEMU ends, which it does on a reset,
and otherwise when the timeout passes with a line not yet reported.
"""

import argparse
import copy
import hashlib
import json
import os
import queue
import re
import shlex
import shutil
import socket
import string
import subprocess
import sys
import tempfile
import threading
import time
import zlib

# README.md's Run command, with the display off, the serial port on
# standard output, a QMP socket for the keys and the pointer, and a
# virtio mouse, so that a relative pointer's events reach a driver too.
# The mouse comes before the tablet: QEMU sends a button or the wheel to
# the device whose driver set DRIVER_OK last of those that take them
# (hw/input/virtio-input.c, virtio_input_set_status; ui/input.c,
# qemu_input_find_handler), and the input drivers start in the order of
# the PCI walk, so the tablet takes them, as it does in the Run command.
# -no-reboot makes a reset end QEMU, so that the lines of a second boot
# cannot complete the first's.
MEMORY = "256M"
DEVICES = ["-net", "none", "-device", "virtio-rng-pci",
           "-device", "virtio-keyboard-pci", "-device", "virtio-mouse-pci",
           "-device", "virtio-tablet-pci", "-no-reboot"]

# The display --size gives QEMU, in place of its default one: a VGA
# device whose EDID offers the size, for the firmware to set as its
# mode. The video memory is more than the default 16 MiB, which a frame
# of 2880 x 1800 pixels of 4 bytes passes; without it the firmware
# offers 800 x 600 (docs/spec/display.md D19).
VIDEO_MEMORY_MB = 32
SIZE = re.compile(r"([1-9][0-9]*)x([1-9][0-9]*)")


def screen_size(text):
    """--size's WIDTHxHEIGHT as a width and a height in pixels."""
    match = SIZE.fullmatch(text)
    if not match:
        raise argparse.ArgumentTypeError(
            "not WIDTHxHEIGHT in pixels: %r" % text)
    return int(match.group(1)), int(match.group(2))


def size_arguments(size):
    """QEMU's arguments for a display of size, a width and a height."""
    return ["-vga", "none", "-device",
            "VGA,edid=on,xres=%d,yres=%d,vgamem_mb=%d"
            % (size[0], size[1], VIDEO_MEMORY_MB)]


# A line of nova-os's log (docs/spec/log.md T1): its component, the
# event's words, and its keys, each value bare or in double quotes. A
# line can follow other output on the serial port, so it is found where
# its component starts.
COMPONENT = r"nova(?:-[a-z0-9-]+)?"
EVENT_LINE = re.compile(r"(%s): ([a-z0-9-]+(?: [a-z0-9-]+)*)"
                        r"((?: [a-z][a-z0-9_]*=(?:\"[^\"]*\"|[^ \"]+))*)$"
                        % COMPONENT)
LINE_START = re.compile(r"%s: " % COMPONENT)
KEY = re.compile(r' ([a-z][a-z0-9_]*)=(?:"([^"]*)"|([^ "]+))')


def find_line(text):
    """The nova-os line in text, as EVENT_LINE matches it where its
    component starts: its groups are the component, the event and the
    text of the keys after it. None if text holds no such line."""
    for start in LINE_START.finditer(text):
        match = EVENT_LINE.match(text, start.start())
        if match:
            return match
    return None


def parse_line(text):
    """The component, event and keys of the nova-os line in text, or
    None."""
    match = find_line(text)
    if not match:
        return None
    return (match.group(1), match.group(2),
            {k.group(1): k.group(2) if k.group(2) is not None
             else k.group(3) for k in KEY.finditer(match.group(3))})


class Event:
    """A line the boot must report: its component and event, with keys,
    each a value it must have, or a pattern it must match whole, and
    keys it must not have. It is reported once, unless repeated says
    more."""

    times = 1

    def __init__(self, line, absent=(), **patterns):
        self.text = line
        self.component, self.event, self.keys = parse_line(line)
        self.keys.update(patterns)
        self.absent = absent

    def repeated(self, times):
        """This event, which the boot must report times times, each by a
        line of its own."""
        counted = copy.copy(self)
        counted.times = times
        return counted

    def matches(self, parsed):
        if not parsed or parsed[:2] != (self.component, self.event):
            return False
        keys = parsed[2]
        for key, wanted in self.keys.items():
            if key not in keys:
                return False
            if isinstance(wanted, re.Pattern):
                if not wanted.fullmatch(keys[key]):
                    return False
            elif keys[key] != wanted:
                return False
        return not any(key in keys for key in self.absent)

    def __str__(self):
        return (self.text + "".join(" no %s" % key for key in self.absent)
                + (" (%d times)" % self.times if self.times > 1 else ""))


# A key's value that may be anything, for a key the line must have.
ANY_VALUE = re.compile(r".+")

RANDOM_PASSED = Event("nova: selftest passed name=random")
STARTUP_DONE = Event("nova: startup done")

# The console's line for each endpoint's description
# (include/input_protocol.h): QEMU's keyboard, its mouse, which has no
# absolute axis, and its tablet, whose axes run from 0 to 0x7FFF
# (include/ui/input.h, INPUT_EVENT_ABS_MAX).
TABLET_MAX = 0x7FFF


def input_lines(component):
    """component's line for each endpoint's description."""
    return [
        Event("%s: input kind=keyboard" % component),
        Event("%s: input kind=pointer" % component, absent=("x", "y")),
        Event("%s: input kind=pointer x=0..%d y=0..%d"
              % (component, TABLET_MAX, TABLET_MAX)),
    ]


INPUT_LINES = input_lines("nova-console")

# The input functions of the machine: the console reports one endpoint
# for each.
INPUT_FUNCTIONS = len(INPUT_LINES)

# The kernel draws the background offscreen while the boot screen's
# spinner turns, hands it to the first process and waits for the
# process's startup report, which follows the console's preparation of
# its panel; it logs how long that preparation took, from the drawing's
# start to the report, in milliseconds to the tenth, rounded down, and
# the frames the spinner started over it, as keys of its background
# line. A turning spinner starts a frame at least every SPINNER_GAP_MS,
# the sum of its parts below, while one other thread is ready, the
# drawing or the console's preparation; each further thread ready
# meanwhile delays it by a slice more. The bound takes the ticks to come
# on time, which under TCG they need not, but there the preparation
# takes over ten times as long. A preparation that may have lasted
# longer than the bound must show MIN_SPINNER_FRAMES; one that ended
# within it may show none.
BACKGROUND = Event("nova: background", prepare_ms=re.compile(r"\d+\.\d"),
                   spinner_frames=re.compile(r"\d+"))
MIN_SPINNER_FRAMES = 1
TENTHS_PER_MS = 10
# The bound's parts, each the kernel's: the spinner's next frame falls
# due within FRAME_MS (kernel/main.c); its wait ends at the first timer
# tick at or after that, and a tick comes every TICK_MS, at TIMER_HZ
# (kernel/x86_64/arch.h); it then waits behind the thread that is
# ready, which keeps the processor for its TIME_SLICE_MS
# (kernel/sched/thread.h) and on to the first tick after the slice
# ends. sched_timer_interrupt (kernel/sched/thread.c) ends the wait and
# the slice at those ticks.
FRAME_MS = 33
TICK_MS = 1
TIME_SLICE_MS = 10
SPINNER_GAP_MS = FRAME_MS + TICK_MS + TIME_SLICE_MS + TICK_MS

# The line of each that draws on the display, the console or the
# compositor, whose screen key is the display's width and height in
# pixels, which --size checks.
SCREEN_SIZE = re.compile(r"\d+x\d+")
DISPLAYS = [Event("nova-console: display", screen=SCREEN_SIZE),
            Event("nova-compositor: display", screen=SCREEN_SIZE)]

# The console's window and the lines before it (docs/spec/display.md D19,
# rules 2 and 4): the compositor's connection for the console, and the
# console's line for the window it made on it, which names the screen it
# was told of and the window's size, the panel's box.
CONSOLE_CONNECTION = Event("nova-compositor: connection issued "
                           "label=console mark=no layers=system rights=none")
CONSOLE_WINDOW = Event("nova-console: window", screen=SCREEN_SIZE,
                       size=SCREEN_SIZE)

# The fallback boot's lines (docs/spec/display.md D19, rule 9). The
# compositor, started on a test disk with the background labelled for a
# display a row taller, refuses it and computes Limb, as the compositor
# of any test disk does. It is started twice, the second time without a
# background, so its display line, the console's connection and window
# and its end come twice, the window before each end, and its first
# process restarts it once and then gives it up. Each input driver is
# started again for the second start, which the first process logs with
# the reason "its client restarted", and may be started again for the
# console, with the reason "its client changed". Only the first is
# required: a driver started for the second start that has not reported
# when the compositor is given up reports straight to the console and is
# not started again, so the second may come for none, some or all of
# the three. The console's keys, which log_in types, are the evidence
# that the console has each driver's endpoint. The console, given no
# background, draws the display after the compositor: its line says so,
# and its backdrop is computed.
COMPOSITOR_STARTS = 2

# The exit code of a compositor that ends for the test disk's COMPFAIL,
# COMPOSITOR_EXIT_TEST in include/display_protocol.h, as the first
# process logs it.
COMPOSITOR_EXIT_TEST = 102

COMPOSITOR_DISPLAY = Event("nova-compositor: display", screen=ANY_VALUE,
                           wallpaper="computed")
REFUSED_BACKGROUND = Event('nova-compositor: background refused '
                           'reason="another size"')

# The compositor's lines on the wallpaper, the console's connection and
# its window, in the system boot: on the system disk, a wallpaper made
# from the kernel's background; on a test disk, whose compositor the
# first process tells of a display a row taller than the background
# (init/console.c, MISMATCH_ROWS), its refusal of the background for
# that, and Limb computed at the display.
COMPOSITOR_DRAWN = {
    False: [Event("nova-compositor: display", screen=ANY_VALUE,
                  wallpaper="section"),
            CONSOLE_CONNECTION, CONSOLE_WINDOW],
    True: [REFUSED_BACKGROUND, COMPOSITOR_DISPLAY, CONSOLE_CONNECTION,
           CONSOLE_WINDOW],
}
COMPOSITOR_ENDED = Event("nova-init: child ended name=compositor exit=%d"
                         % COMPOSITOR_EXIT_TEST)
COMPOSITOR_RESTARTED = Event("nova-init: child restarted name=compositor")
COMPOSITOR_GIVEN_UP = Event("nova-init: child given up name=compositor")
INPUT_RESTARTED = [
    Event("nova-init: child restarted name=virtio-input",
          reason="its client restarted"),
    Event("nova-init: child restarted name=virtio-input",
          reason="its client changed"),
]
CONSOLE_AFTER = Event("nova-console: display backdrop=computed "
                      "after=compositor", screen=ANY_VALUE)
FALLBACK_LINES = (
    [REFUSED_BACKGROUND,
     COMPOSITOR_DISPLAY.repeated(COMPOSITOR_STARTS),
     CONSOLE_CONNECTION.repeated(COMPOSITOR_STARTS),
     CONSOLE_WINDOW.repeated(COMPOSITOR_STARTS),
     COMPOSITOR_ENDED.repeated(COMPOSITOR_STARTS),
     COMPOSITOR_RESTARTED, COMPOSITOR_GIVEN_UP]
    + [INPUT_RESTARTED[0].repeated(INPUT_FUNCTIONS)]
    + [CONSOLE_AFTER])

# The no-answer boot's lines (docs/spec/display.md D3 and D19, rules 2
# and 9). The compositor of noanswer.img takes the display, serves its
# input endpoints and answers no connection, so the first process's wait
# for the console's connection passes its deadline at each of the
# compositor's two starts: it logs that the wait failed with
# ERR_TIMED_OUT and ends the compositor with task_kill, whose end it logs
# with PROCESS_EXIT_KILLED (include/native_abi.h). It restarts the
# compositor once and then gives it up, as the fallback boot's, and the
# input drivers and the console follow as they do there. The compositor's
# line for each input endpoint comes from its first start alone: the
# drivers are started again for the second only once its wait is over.
ERR_TIMED_OUT = 10
PROCESS_EXIT_KILLED = 0x100
CONNECTION_UNANSWERED = Event("nova-init: connection wait failed status=%d"
                              % ERR_TIMED_OUT)
COMPOSITOR_KILLED = Event("nova-init: child ended name=compositor exit=%d"
                          % PROCESS_EXIT_KILLED)

# How long the first process waits for the compositor's answer to a
# connection, COMPOSITOR_CONNECT_DEADLINE_SECONDS in init/compositor.h:
# the no-answer boot sits through one such wait for each start.
CONNECT_DEADLINE_SECONDS = 30

STARTUP_LINES = [
    Event("nova-store: serving"),
    STARTUP_DONE,
    BACKGROUND,
]

# The system boot's lines before the compositor's (COMPOSITOR_DRAWN): the
# compositor has the input endpoints. The fallback boot's console has them
# once the compositor is given up.
SYSTEM_LINES = STARTUP_LINES + input_lines("nova-compositor")
FALLBACK_STARTUP_LINES = STARTUP_LINES + INPUT_LINES
NO_ANSWER_LINES = (
    input_lines("nova-compositor")
    + [REFUSED_BACKGROUND,
       COMPOSITOR_DISPLAY.repeated(COMPOSITOR_STARTS),
       CONNECTION_UNANSWERED.repeated(COMPOSITOR_STARTS),
       COMPOSITOR_KILLED.repeated(COMPOSITOR_STARTS),
       COMPOSITOR_RESTARTED, COMPOSITOR_GIVEN_UP]
    + [INPUT_RESTARTED[0].repeated(INPUT_FUNCTIONS)]
    + [CONSOLE_AFTER])

# The window boot's lines (docs/spec/display.md D19, rules 2, 4, 5 and 8,
# and test/wintest.h). The compositor, started on a test disk, refuses the
# background for a display a row taller and computes Limb, as the fallback
# boot's does. The first process issues the test client's connection on
# the SYSTEM layer; the client names the screen it was told of and the
# size of the window it was given, makes two buffers, and logs each frame
# that shows a present, the second after the release of the first's
# buffer, a bit of the mask of buffers. The window is on screen once the
# client logs the second frame; the screen is then checked. The input
# drivers' endpoints are the compositor's, which logs a line for each as
# the console does, and the pointer's events and the keys that the boot
# then sends are those of pointer_steps and WINDOW_KEYS below.
WINDOW_SHOWN = Event("nova-wintest: shown frame=2")
WINDOW_MADE = Event("nova-wintest: window", screen=SCREEN_SIZE,
                    size=SCREEN_SIZE)
WINDOW_LINES = STARTUP_LINES + [
    REFUSED_BACKGROUND,
    COMPOSITOR_DISPLAY,
    Event("nova-compositor: connection issued number=1 label=wintest "
          "mark=no layers=system rights=none"),
    WINDOW_MADE,
    Event("nova-wintest: shown frame=1"),
    Event("nova-wintest: released buffers=1"),
    WINDOW_SHOWN,
] + input_lines("nova-compositor")

# The lines the boots that run on the compositor refuse besides those that
# report a failure. The client of the window boot and the console of the
# others hold their window and their connection for the whole boot, and
# a close by their holder logs nothing (docs/spec/display.md D19, rule 8),
# so each is a close the compositor made. The console is handed no
# endpoint while the compositor runs and logs none for a window's keys
# and wheel (docs/spec/console.md C2), so a line of its for an input or a
# pointer event means the endpoint went to it, and a compositor that
# refuses a message of an endpoint has failed to serve it.
COMPOSITOR_REFUSED = [Event("nova-compositor: window closed"),
                      Event("nova-compositor: connection closed"),
                      Event("nova-compositor: input message refused"),
                      Event("nova-console: input")] + [
    Event("nova-console: pointer " + change)
    for change in ("move", "press", "release", "wheel")]

# The window boot's console draws, connects and takes a key not at all,
# since the test client is the compositor's one.
WINDOW_REFUSED = COMPOSITOR_REFUSED + [Event("nova-console: window")]

# The no-answer boot's compositor answers no connection, so a line of its
# for one issued, or of the console's for a window, means it answered.
NO_ANSWER_REFUSED = [CONSOLE_CONNECTION, CONSOLE_WINDOW]

# The modes whose console draws in a window on the compositor, through
# which the boot types and points (docs/spec/display.md D19, rule 2).
WINDOW_PATH_MODES = ("system", "persist", "logout")

# The modes whose compositor is given up, so that the console takes the
# display and the boot types and points through the console alone (rule
# 9).
GIVEN_UP_MODES = ("fallback", "noanswer")

CHAIN_LINES = [STARTUP_DONE]

# The specimen's boot reports what a system boot does up to the
# display's release, and then the specimen drawing in the console's
# place.
SPECIMEN_LINES = [
    Event("nova-store: serving"),
    STARTUP_DONE,
    BACKGROUND,
    Event("nova-init: display given",
          path=re.compile(r"/store/[^/]+-specimen/bin/specimen\.exe")),
    Event("nova-specimen: drawn", screen=ANY_VALUE),
]

SPECIMEN_PROTOTYPE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "..", "design", "text", "specimen.py")
SCREEN_COPY = "screen.ppm"
PROTOTYPE_COPY = "prototype.ppm"
EXPECTED_COPY = "expected.ppm"

# The tests a test disk's report must hold, passed and not skipped, by
# their names from the report's top (docs/spec/log.md T2): the kernel's
# self-tests, the first process's, with the data disk's check only with
# a data disk whose GPT is whole and the chain only from chain.img, and
# in the chain, each step, named by the file it writes, the links among
# them those nova-nix pins.
KERNEL_TESTS = ["kernel"] + ["kernel/" + name
                             for name in ("page", "random", "object",
                                          "thread")]
INIT_TESTS = ["init", "init/calls", "init/hello", "init/boot-disk"]
FSTEST = "init/fstest"
CHAIN = "init/chain"
CHAIN_STEPS = 20
CHAIN_LINKS = ["hex0", "hex1", "hex2", "catm", "M0", "cc_x86", "M2", "M1"]
# A link's passing result carries "pinned" and its name, which shows its
# pin was checked.
CHAIN_LINK_DATA = {"%s/%s.exe" % (CHAIN, link): "pinned " + link
                   for link in CHAIN_LINKS}

# ci/screens.c, which draws the screens a boot must show on the host with
# the guest's own drawing code, built as its header gives with the
# host's C compiler, CC if set; and how long a step's screen may take to
# show (docs/spec/display.md D19).
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SCREENS_SOURCES = ["ci/screens.c", "console/text.c", "console/view.c",
                   "console/panel.c", "lib/compose.c", "lib/outline.c",
                   "lib/window.c", "gfx/gfx.c", "gfx/font.c",
                   "gfx/raster.c", "gfx/limb.c"]
SCREENS_INCLUDES = ["include", "gfx", "console", "lib", "test"]
SCREENS_FLAGS = ["-std=c11", "-O2", "-Wall", "-Wextra", "-pedantic",
                 "-Werror"]
SCREENS_PROGRAM = "screens"
CONSOLE_SCREEN = "console"
CONSOLE_WINDOW_SCREEN = "console-window"
LIMB_SCREEN = "limb"
WINDOW_SCREEN = "window"
TYPED_LINE = "line"
WRITTEN = "write"
WHEEL_TURNED = "wheel"
CARET = "caret"
CARET_PHASES = ["on", "off"]
CARET_STEADY = ["on"]
SCREEN_TIMEOUT = 60
# How long a screen must stay as it is where the caret is steady. The
# caret blinks with a lit half and a hidden half of CARET_BLINK_NS
# (console/console.c), half a second each, so a blinking caret shows
# its hidden half within a second; three seconds allow a console that
# takes twice as long to wake, as one under emulation may.
STEADY_SECONDS = 3
PPM_MAGIC = b"P6"
PPM_MAXVAL = b"255"
PPM_FIELDS = 4

DEFAULT_TIMEOUTS = {"system": 300, "fallback": 300, "window": 300,
                    "persist": 300, "chain": 2700, "specimen": 300,
                    "logout": 300}
# The no-answer boot is the fallback boot's with the waits it sits through.
DEFAULT_TIMEOUTS["noanswer"] = (
    DEFAULT_TIMEOUTS["fallback"]
    + COMPOSITOR_STARTS * CONNECT_DEADLINE_SECONDS)

DATA_LINES = [
    Event("nova-fs: pool opened", lba=ANY_VALUE),
    Event("nova-init: data published"),
]

# What --corrupt-gpt changes in the data disk (UEFI 2.11 5.3), in the
# primary table at LBA 1 and the backup it names: a byte of each
# header's HeaderCRC32, or of the first entry's PartitionName in each
# entry array, which only a CRC check refuses; or, with each array's
# CRC and each header's made to match, the first entry, the pool's as
# mkdata writes it, moved to start at LBA 0; or moved to start at LBA 1,
# the header's, with each header's FirstUsableLBA moved there too, so
# that only the header's own placing of its usable LBAs is wrong; or a
# second entry, a Linux filesystem's, over the same blocks: which only
# the checks of 5.3.1 refuse. The usable range and the entry both moved
# to LBA 0 instead kept EDK2 from booting the system disk at all. Both
# tables are changed because firmware restores a primary table that
# fails from a backup that passes, as 5.3.2 asks and EDK2 does on a
# writable disk.
SECTOR = 512
PRIMARY_LBA = 1
HEADER_SIZE = 12
HEADER_CRC = 16
ALTERNATE_LBA = 32
FIRST_USABLE = 40
ENTRIES_LBA = 72
ENTRY_COUNT = 80
ENTRY_SIZE = 84
ENTRIES_CRC = 88
ENTRY_UNIQUE = 16
ENTRY_FIRST = 32
ENTRY_LAST = 40
ENTRY_NAME = 56
GUID_SIZE = 16
LINUX_FILESYSTEM = bytes.fromhex("af3dc60f838472478e793d69d8477de4")
GPT_REFUSED = {
    "header": Event("nova-fs: gpt refused check=header"),
    "entries": Event("nova-fs: gpt refused check=entries"),
    "extent": Event("nova-fs: gpt refused check=extent"),
    "usable": Event("nova-fs: gpt refused check=layout"),
    "overlap": Event("nova-fs: gpt refused check=extent"),
}
DATA_REFUSED = Event("nova-init: data unpublished")
LOGIN_ABSENT = Event('nova-init: login unavailable reason="no /users"')

# The pool format, version 1, as include/pool_format.h numbers it: the
# pool partition's type GUID in GPT's byte order; a superblock's fields;
# a block pointer's; the root block's; a node record's; a directory
# entry's; and the node kinds.
POOL_TYPE = bytes([0x24, 0x8f, 0x76, 0xb1, 0x5a, 0x22, 0x6d, 0x43,
                   0xaa, 0xbd, 0x66, 0x9b, 0x87, 0x4b, 0xac, 0x7f])
POOL_BLOCK = 4096
POOL_RING_SLOTS = 4
DIGEST_SIZE = 32
POOL_MAGIC = b"NOVAPOOL"
POOL_VERSION = 1
SUPER_VERSION = 8
SUPER_BLOCK_SIZE = 12
SUPER_BLOCKS = 16
SUPER_TXG = 24
SUPER_ROOT = 32
SUPER_DIGEST = 96
POINTER_FIRST = 0
POINTER_SECOND = 8
POINTER_DIGEST = 32
POINTER_SIZE = 64
POOL_FANOUT = POOL_BLOCK // POINTER_SIZE
ROOT_TABLE_LEVELS = 16
ROOT_TABLE = 24
RECORD_KIND = 0
RECORD_LEVELS = 2
RECORD_SIZE_FIELD = 8
RECORD_ROOT = 16
RECORD_SIZE = 128
RECORDS_PER_BLOCK = POOL_BLOCK // RECORD_SIZE
ROOT_SLOT = 0
POOL_ENTRY_SLOT = 0
POOL_ENTRY_KIND = 4
POOL_ENTRY_LENGTH = 5
POOL_ENTRY_NAME = 6
DIRECTORY = "directory"
FILE = "file"
KINDS = {1: DIRECTORY, 2: FILE}

# The account the boots make and log in with: a name and a password
# typed as QMP's send-key names their keys. The name is logged, as the
# accounts service and the shell log it; the password must never be,
# and is typed once more at the shell's prompt, where a person who took
# a drawn prompt for login's would type it, to check that the shell
# keeps it out of the log too. It has no space, so that a log of the
# first word typed would hold all of it.
ACCOUNT_NAME = "ci"
ACCOUNT_PASSWORD = "civerifies"
QCODES = {" ": "spc", ".": "dot", "/": "slash"}

# The account's home on the data disk's volume, which must be the only
# entry of /users.
USERS = ("users",)
HOME = USERS + (ACCOUNT_NAME,)

# What login and the accounts service report before login reads its
# first line, and then on the first boot and on a later one.
# The accounts service's self-test of BLAKE2b and Argon2id
# (docs/spec/log.md T3) is reported before its count.
ACCOUNTS_PASSED = Event("nova-accounts: selftest passed name=argon2id")
LOGIN_READY = Event("nova-login: ready session=console")
FIRST_BOOT_READY = [ACCOUNTS_PASSED, Event("nova-accounts: ready accounts=0"),
                    LOGIN_READY]
LATER_BOOT_READY = [ACCOUNTS_PASSED, Event("nova-accounts: ready accounts=1"),
                    LOGIN_READY]
SESSION = Event("nova-shell: session name=%s role=administrator"
                % ACCOUNT_NAME)
CREATED = Event("nova-accounts: account created name=%s id=1 "
                "role=administrator" % ACCOUNT_NAME)
LOGGED_IN = Event("nova-accounts: login accepted name=%s" % ACCOUNT_NAME)
WHOAMI = "whoami"
WHOAMI_RAN = Event("nova-shell: whoami name=%s id=1 role=administrator"
                   % ACCOUNT_NAME)
NO_COMMAND = Event("nova-shell: command unknown user=%s" % ACCOUNT_NAME)

# The session ends with logout. Login then ends every job it made for the
# session's programs, and reports how many of them still ran; the shell
# is the session's only program here, and login did not make its job, so
# none did.
LOGOUT = "logout"


def session_ended(killed):
    """Login's line for the end of the session, with killed programs
    stopped."""
    return Event("nova-login: session ended name=%s killed=%d"
                 % (ACCOUNT_NAME, killed))

# The file commands the first boot's session runs in its home, each as
# the lines typed for it, the status the shell must log for it, in a
# line that names the command and the status alone, and the tree the
# home must then hold on the data disk, read on the host: each path
# below the home, with a file's bytes or DIRECTORY. A directory is made,
# a file written and written again, which replaces it, shown and moved,
# an empty file written and removed; then a directory is refused under
# /store, which the session holds only to read, and a file in another
# user's home, which it holds no handle to. A later boot shows the file
# again. Each command's own flush is what puts its change on the disk:
# the shell logs the status once the flush is answered, and nothing
# else writes the volume while the session runs.
STATUS_RAN = "nova-shell: command ended user=%s name=%%s status=%%d" % (
    ACCOUNT_NAME)
OK = 0
ERR_ACCESS_DENIED = 6
ERR_NOT_FOUND = 16
END_OF_LINES = "."
FIRST_DRAFT = "apricot compote"
FINAL_LINES = ["quince marmalade", "damson chutney"]


def file_of(lines):
    return "".join(line + "\n" for line in lines).encode("ascii")


MADE = {"notes": DIRECTORY}
DRAFTED = {**MADE, "notes/draft": file_of([FIRST_DRAFT])}
REPLACED = {**MADE, "notes/draft": file_of(FINAL_LINES)}
MOVED = {**MADE, "notes/final": file_of(FINAL_LINES)}
STALE = {**MOVED, "notes/stale": file_of([])}
FILE_COMMANDS = [
    (["mkdir notes"], "mkdir", OK, MADE),
    (["write notes/draft", FIRST_DRAFT, END_OF_LINES], "write", OK, DRAFTED),
    (["write notes/draft"] + FINAL_LINES + [END_OF_LINES], "write", OK,
     REPLACED),
    (["cat notes/draft"], "cat", OK, REPLACED),
    (["mv notes/draft notes/final"], "mv", OK, MOVED),
    (["write notes/stale", END_OF_LINES], "write", OK, STALE),
    (["rm notes/stale"], "rm", OK, MOVED),
    (["mkdir /store/notes"], "mkdir", ERR_ACCESS_DENIED, MOVED),
    (["write /users/nobody/notes"], "write", ERR_NOT_FOUND, MOVED),
]
LATER_FILE_COMMANDS = [(["cat notes/final"], "cat", OK, MOVED)]

# The wheel's check (docs/spec/display.md D19, phase 1). In a system
# boot the session then writes a file of SCROLL_LINES, each echoed on a
# line of the console's panel of its own, as many as the panel's 30
# lines and the 6 the turns below scroll back, so that the view turned
# back shows lines typed alone, which ci/screens.c draws. Each is
# one character because QEMU queues at most 1,024 key events, syncs and
# delays, six a key, and drops a key past them (ui/input.c, queue_limit,
# qmp_send_key; ui/input-legacy.c in 8.2.2), so the command and its
# lines, typed before the status it logs is awaited, stay under 170
# keys; the home must then hold the file whole, so no key was dropped.
SCROLL_LINES = list(string.ascii_lowercase + string.digits)
SCROLL_FILE = "notes/alphabet"
SCROLL_COMMAND = (["write " + SCROLL_FILE] + SCROLL_LINES + [END_OF_LINES],
                  "write", OK, {**MOVED, SCROLL_FILE: file_of(SCROLL_LINES)})

# The shell's prompt after the write, which ci/screens.c draws on the
# line after the dot: the account's name, "@nova ", the working
# directory, its home, and "> " (shell/shell.c, prompt). The screen with
# it drawn and its read waiting is matched before the wheel turns, since
# the shell logs the write's status before it writes the prompt, and a
# write brings the view back to the end.
PROMPT = "%s@nova /%s> " % (ACCOUNT_NAME, "/".join(HOME))

# What login has written when it waits for a name: on a first boot, which
# has no account, its message and the prompt for the new account's name,
# and on a later boot the prompt to log in (login/login.c).
FIRST_PROMPT = ("nova-os has no accounts yet. Create the administrator's "
                "account.\nname: ")
LATER_PROMPT = "login: "

# The secure attention key, Control and Alt held and Delete pressed and
# released, as key_event takes them (docs/spec/display.md D12).
ATTENTION_KEYS_SENT = "ctrl+ alt+ delete+ delete- alt- ctrl-"

# The wheel's turns, each detent a press and release of QMP's wheel-up or
# wheel-down in one input-send-event, which QEMU sends as REL_WHEEL 1 or
# -1 and a release no driver takes, ended by one SYN_REPORT
# (hw/input/virtio-input-hid.c, ui/input.c), with the screen matched
# after each turn: two detents back, one forward, two more forward,
# which the end stops after one, and two back, which show again what the
# first two did. Each screen differs from every one the turns before it
# leave on the screen, so it matches only once the console has taken
# them.
WHEEL_TURNS = [2, -1, -2, 2]

# The pointer's events, each one QMP input-send-event command, which
# QEMU ends with one SYN_REPORT (ui/input.c, qmp_input_send_event): the
# tablet moved to POINTER_AT, whose numbers no other line of the log
# holds; each button the console names pressed and released; the wheel
# turned up and down, each turn a press, which QEMU sends as REL_WHEEL,
# and a release, which it sends as a button no driver takes
# (hw/input/virtio-input-hid.c); and the mouse moved, which alone takes
# relative motion. Then the line the console must log for each.
POINTER_AT = {"x": 13579, "y": 24680}
BUTTONS = ["left", "right", "middle"]


def button(name, down):
    return [{"type": "btn", "data": {"button": name, "down": down}}]


def move(kind, at):
    return [{"type": kind, "data": {"axis": axis, "value": value}}
            for axis, value in at.items()]


POINTER_EVENTS = ([move("abs", POINTER_AT)]
                  + [button(name, down)
                     for name in BUTTONS + ["wheel-up", "wheel-down"]
                     for down in (True, False)]
                  + [move("rel", {"x": 7, "y": -3})])


def pointer_lines(component):
    """The line component must log for each of POINTER_EVENTS."""
    return ([Event("%s: pointer move mode=absolute axes=xy" % component)]
            + [Event("%s: pointer %s button=%s" % (component, change, name))
               for name in BUTTONS
               for change in ("press", "release")]
            + [Event("%s: pointer wheel direction=up" % component),
               Event("%s: pointer wheel direction=down" % component),
               Event("%s: pointer move mode=relative axes=xy" % component)])


POINTER_LINES = pointer_lines("nova-console")
COMPOSITOR_POINTER_LINES = pointer_lines("nova-compositor")

# The tablet's place when the console's window is turned the wheel: inside
# the window at every size the boots run, whose middle D6 puts half the
# screen across and at 40 percent of its height down (display.md D6,
# Placement), and a place no other line holds (UNLOGGED).
WHEEL_AT = {"x": 16301, "y": 13007}

# The keys the window boot sends, each string one QMP input-send-event
# of key events in the order given, a key's name (its QKeyCode) and a
# plus for pressed or a minus for released: input-send-event, not
# send-key, since send-key releases the keys in an order of its own. It
# sends presses and releases, so the boot exercises no repeat.
# What each should do, per docs/spec/display.md D11 and D12, and the
# strings in order: a letter; a capital; a symbol, which Shift gives; a
# space, which a log value must quote; Left, which gives no text; Control
# with C and Alt with X, which give none; Delete alone, which reaches the
# window; Delete with Control, and the keypad's with Alt, which reach it
# too; Delete pressed before Control and Alt, which is no secure attention
# key; the same with Delete pressed again while the first press is held,
# which is the secure attention key, and ends what the window was sent of
# Delete, so that its release is not sent; the secure attention key with the left keys released in the order
# pressed, as the release of Delete then comes after Control's and Alt's;
# the same, then Delete pressed again with no Control or Alt held, a press
# of its own which the window is sent with its release; the key on the
# keypad with Shift and the right keys, released in reverse; and a last
# letter, whose release the boot waits for, since every event before it
# has then been taken.
WINDOW_KEYS = [
    "a+ a-",
    "shift+ a+ shift- a-",
    "shift+ 1+ 1- shift-",
    "spc+ spc-",
    "left+ left-",
    "ctrl+ c+ c- ctrl-",
    "alt+ x+ alt- x-",
    "delete+ delete-",
    "ctrl+ delete+ delete- ctrl-",
    "alt+ kp_decimal+ kp_decimal- alt-",
    "delete+ ctrl+ alt+ delete- alt- ctrl-",
    "delete+ ctrl+ alt+ delete+ delete- alt- ctrl-",
    "ctrl+ alt+ delete+ ctrl- alt- delete-",
    "ctrl+ alt+ delete+ ctrl- alt- delete+ delete-",
    "shift+ ctrl_r+ alt_r+ kp_decimal+ kp_decimal- alt_r- ctrl_r- shift-",
    "z+ z-",
]

# For each key those name, its evdev code and the characters it gives
# unshifted and shifted, or None (include/input_protocol.h and
# docs/spec/console.md C3's table), and the modifier a key is, as
# display.md D11's rule 2 names them.
KEY_CODES = {
    "a": (30, "a", "A"), "c": (46, "c", "C"), "x": (45, "x", "X"),
    "z": (44, "z", "Z"), "1": (2, "1", "!"), "spc": (57, " ", " "),
    "left": (105, None, None), "delete": (111, None, None),
    "kp_decimal": (83, None, None), "shift": (42, None, None),
    "ctrl": (29, None, None), "alt": (56, None, None),
    "ctrl_r": (97, None, None), "alt_r": (100, None, None),
}
KEY_MODIFIERS = {"shift": "shift", "ctrl": "control", "ctrl_r": "control",
                 "alt": "alt", "alt_r": "alt"}
MODIFIER_ORDER = ["shift", "control", "alt", "super"]
NO_TEXT_MODIFIERS = {"control", "alt", "super"}
ATTENTION_KEYS = ["delete", "kp_decimal"]
ATTENTION_MODIFIERS = {"control", "alt"}
KEY_LINE = "nova-wintest: key"
ATTENTION_LINE = "nova-compositor: attention"
TIME_MS = re.compile(r"\d+\.\d")

# The compositor's budget lines (docs/spec/display.md D19, rule 8), which
# a test disk's compositor writes for the disk's flag file BUDGETS: one
# kind for the frames the pointer's motion alone damaged and one for
# those a window's present damaged, each written once BUDGET_FRAMES
# frames of its kind have been composed since its last. BUDGET_FORM is
# the text of a line after its component and its event, whole: its keys,
# each once, bare, in this order and no other: the kind; the frames it
# counts; the most pixels one of them composed, which is at least one,
# since a frame that is counted was damaged; and their times together
# and the longest, in milliseconds to the tenth.
BUDGET_LINE = ("nova-compositor", "budget")
BUDGET_FRAMES = 16
BUDGET_POINTER = "pointer"
BUDGET_PRESENT = "present"
BUDGET_FORM = re.compile(
    r" change=(?P<change>%s|%s) frames=(?P<frames>%d)"
    r" pixels=(?P<pixels>[1-9][0-9]*)"
    r" total_ms=(?P<total_ms>%s) max_ms=(?P<max_ms>%s)"
    % (BUDGET_POINTER, BUDGET_PRESENT, BUDGET_FRAMES, TIME_MS.pattern,
       TIME_MS.pattern))

# What check_budgets sends to have the compositor compose frames of each
# kind (display.md D9's first two rows). For the pointer, the tablet moved
# between two places the boot keeps out of the log (UNLOGGED): POINTER_AT,
# below the console's window, where point puts the tablet first in every
# boot, and WHEEL_AT, inside the window, where scroll puts it in a boot
# that checks the wheel; a boot that does not, the persistence boot,
# sends WHEEL_AT here first. At every size the boots run the two lie more
# than a third of the screen's height apart, far more than the pointer's
# image is high, so the image's box at one shares no pixel with its box
# at the other, and a frame composes two images.
# For a present, a letter typed at the shell's prompt and Backspace,
# which erases it: the console presents the cells each changes, and the
# prompt is left as it was. Each item is one QMP input-send-event.
BUDGET_MOVES = [POINTER_AT, WHEEL_AT]
BUDGET_TYPED = ["a+ a-", "backspace+ backspace-"]

# How many lines of a kind check_budgets waits for: a line counts the
# BUDGET_FRAMES frames of its kind before it, whenever they were composed,
# so the first may count frames from before the boot's own moves or keys,
# and the second counts those alone.
BUDGET_LINES = 2

# How long check_budgets reads the log after each move and each key
# before it sends the next, in seconds: three of the compositor's frames
# of 16.7 ms (FRAME_NS in interfaces/display.desc), so that each is
# composed in a frame of its own where the guest keeps up. Where it does
# not, changes that share a frame are counted once, and a letter erased
# before the console drew it makes no frame; check_budgets goes on until
# the lines have come, so the pace sets how long that takes and not
# whether it does.
BUDGET_PACE = 0.05


def describe(parsed):
    """A parsed line as text."""
    component, event, keys = parsed
    return "%s: %s%s" % (component, event, "".join(
        " %s=%s" % item for item in keys.items()))


def describe_event(event):
    """An Event as the line it asks for, a pattern given as its text."""
    return "%s: %s%s" % (event.component, event.event, "".join(
        " %s=%s" % (key, value.pattern if isinstance(value, re.Pattern)
                    else value) for key, value in event.keys.items()))


def modifiers_held(held):
    """The names of the modifiers the keys in held make, in the order
    the window test client logs them, or none."""
    names = {KEY_MODIFIERS[key] for key in held}
    return [name for name in MODIFIER_ORDER if name in names]


def key_script(script):
    """The window test client's key lines the keys of script must make,
    an Event for each in order, and, for each press of the secure
    attention key, how many of them come before it. A model of the keys
    written from display.md D11 and D12 and not from the code it checks:
    the secure attention key is a press of Delete or the keypad's with
    Control and Alt held, which goes to no window; a repeat or a release
    goes to a window only if its press did, so the attention key's go to
    none, wherever they come, and so do a later one's of a code whose
    earlier press the window was sent, since the attention key's press
    ends that; every press that is not the attention key's is sent, with
    the modifiers held after it, and the text of a character key pressed
    with no Control, Alt or Super held."""
    held, sent, events, attentions = [], set(), [], []
    for step in script:
        for item in step.split():
            key, down = item[:-1], item.endswith("+")
            code, plain, shifted = KEY_CODES[key]
            before = modifiers_held(held)
            if key in KEY_MODIFIERS:
                if down:
                    held.append(key)
                else:
                    held.remove(key)
            if (down and key in ATTENTION_KEYS
                    and ATTENTION_MODIFIERS <= set(before)):
                attentions.append(len(events))
                sent.discard(key)
            elif down or key in sent:
                after = modifiers_held(held)
                text = ""
                if down and plain and not NO_TEXT_MODIFIERS & set(after):
                    text = shifted if "shift" in after else plain
                events.append(Event(
                    KEY_LINE, code=str(code),
                    value="pressed" if down else "released",
                    modifiers=",".join(after) or "none", text=text,
                    time_ms=TIME_MS))
                if down:
                    sent.add(key)
                else:
                    sent.discard(key)
    return events, attentions


class KeyScript:
    """The lines a boot's keys must make (key_script), checked as the
    Serial reads them: the window test client's key lines, each the next
    of the events, so that a key that reaches the window out of order,
    twice or not as the secure attention key must not fails the boot at
    its line; and the compositor's attention lines, one for each press of
    that key, each before the client's line for the first key sent after
    it, since the compositor logs it before it queues that key, and no
    more of them than there are presses. Only the lines read while the
    boot waits are checked, which is up to the client's line for the
    last key sent (drive_window): a key line or an attention line that
    comes after it is not read for this. An attention line names no key,
    so it is tied to a press of the secure attention key by its place
    among the attention lines and by its time, not by what it says. The
    client may log a key later than the compositor logs a later one, so
    no other order is required. Each line gives its time, and finish
    checks the times of the client's lines do not fall from one to the
    next, and that each attention line's is not before that of the
    client's line for the last key sent before it and not after that of
    its line for the first key sent after it, where there are such keys.
    It checks no other time. The pointer does not move while the keys are
    typed, so a pointer line of the client fails the boot."""

    def __init__(self, events, attentions):
        self.events = events
        self.attentions = attentions
        self.client = []
        self.logged = []

    def take(self, parsed):
        """Check a line, parsed, of the boot."""
        if parsed[:2] == tuple(KEY_LINE.split(": ")):
            self.take_key(parsed)
        elif parsed[:2] == tuple(ATTENTION_LINE.split(": ")):
            self.take_attention(parsed)
        elif parsed[0] == "nova-wintest" and parsed[1] in (
                CLIENT_POINTER_EVENTS):
            raise BootFailed("the window was sent a pointer event while the "
                             "keys were typed: %s" % describe(parsed))

    def take_key(self, parsed):
        index = len(self.client)
        if index >= len(self.events):
            raise BootFailed("a key line after the last that was sent: %s"
                             % describe(parsed))
        if not self.events[index].matches(parsed):
            raise BootFailed("key line %d is %s; the window should be sent "
                             "%s" % (index + 1, describe(parsed),
                                     describe_event(self.events[index])))
        due = len([n for n in self.attentions if n <= index])
        if len(self.logged) < due:
            raise BootFailed("key line %d, %s, came before the compositor "
                             "logged the secure attention key that precedes "
                             "it" % (index + 1, describe(parsed)))
        self.client.append(float(parsed[2]["time_ms"]))

    def take_attention(self, parsed):
        if len(self.logged) == len(self.attentions):
            raise BootFailed("the compositor logged the secure attention "
                             "key more often than it was pressed: %d times"
                             % (len(self.logged) + 1))
        self.logged.append(float(parsed[2]["at_ms"]))

    def finish(self):
        """Check that every line has been read, and the times."""
        if (len(self.client) != len(self.events)
                or len(self.logged) != len(self.attentions)):
            raise BootFailed("%d of %d key lines and %d of %d attention "
                             "lines were read" % (
                                 len(self.client), len(self.events),
                                 len(self.logged), len(self.attentions)))
        for earlier, later in zip(self.client, self.client[1:]):
            if later < earlier:
                raise BootFailed("a key's time, %.1f ms, is before the one "
                                 "sent before it, %.1f ms" % (later, earlier))
        for number, (count, at) in enumerate(zip(self.attentions,
                                                  self.logged)):
            if count > 0 and at < self.client[count - 1]:
                raise BootFailed("the secure attention key %d at %.1f ms is "
                                 "before the key pressed before it, at "
                                 "%.1f ms" % (number + 1, at,
                                              self.client[count - 1]))
            if count < len(self.client) and at > self.client[count]:
                raise BootFailed("the secure attention key %d at %.1f ms is "
                                 "after the key pressed after it, at %.1f "
                                 "ms" % (number + 1, at, self.client[count]))

# The pointer the window boot moves over the test client's window
# (docs/spec/display.md D10, D11 and D19's rule 11), as steps: what each
# sends through QMP, one input-send-event for each item of events, what the
# window must then be sent, the pointer's place the screen must show after
# it, if any, and, if it is to, that the screen must hold still for
# STEADY_SECONDS. A place is a tablet's two values, which the tablet gives
# its 0 to TABLET_MAX, and are chosen against the window's place, which D6
# gives for a window half the screen across and high (test/wintest.h) on a
# screen of any size the boots run: OUTSIDE lies left of and above the
# window, ENTRY, MOVED and DRAG inside it, BELOW left of and below it and
# LAST inside it. The values are ones no other line of the log holds
# (UNLOGGED).
OUTSIDE = {"x": 4001, "y": 2003}
ENTRY = {"x": 15991, "y": 13003}
MOVED = {"x": 11971, "y": 9007}
DRAG = {"x": 20011, "y": 17009}
BELOW = {"x": 2003, "y": 29017}
LAST = {"x": 13999, "y": 11003}
POINTER_PLACES = [OUTSIDE, ENTRY, MOVED, DRAG, BELOW, LAST]
RELATIVE_MOTION = {"x": 7, "y": -3}

# The units of a pointer's place and of the wheel as a window is sent them
# (DISPLAY_POSITION_ONE and DISPLAY_WHEEL_DETENT in
# include/display_protocol.h; display.md D10, rules 4 and 5), and the size
# of the tablet's axis, its values 0 to TABLET_MAX.
POSITION_ONE = 256
WHEEL_DETENT = 120
TABLET_RANGE = TABLET_MAX + 1

# The window's strip and the share of the screen's height its middle is put
# at (display.md D6, Placement), in percent.
STRIP = 0
PLACEMENT_PERCENT = 40

# The lines the test client logs for the pointer (test/wintest.c).
ENTER_LINE = "nova-wintest: enter"
POINTER_LINE = "nova-wintest: pointer"
LEAVE_LINE = "nova-wintest: leave"
CLIENT_POINTER_EVENTS = ("enter", "pointer", "leave")
COMPOSITOR = "nova-compositor"

# The pointer's pixel is passed to ci/screens.c for the window's screen.
POINTER_INPUT = "pointer"


class Geometry:
    """The screen and the window on it, in pixels, as the test client logs
    them in its window line, and the window's place, which display.md D6
    gives: the left is (W - w) / 2, and the top is
    S + (H - S) * 40 / 100 - h / 2, raised to at least S and lowered to at
    most H - h where that is not below S, in integers rounded down."""

    def __init__(self, screen, size):
        self.screen_width, self.screen_height = screen
        self.width, self.height = size
        self.left = (self.screen_width - self.width) // 2
        top = (STRIP + (self.screen_height - STRIP) * PLACEMENT_PERCENT // 100
               - self.height // 2)
        top = max(top, STRIP)
        if self.screen_height - self.height >= STRIP:
            top = min(top, self.screen_height - self.height)
        self.top = top

    @classmethod
    def of(cls, keys):
        """The geometry of the keys of the client's window line."""
        return cls(size_of(keys["screen"]), size_of(keys["size"]))

    def place(self, at):
        """The tablet place at on the screen, across and down, in 256ths
        of a pixel and rounded down: display.md D10, rule 4, with an axis
        that runs from 0 to TABLET_MAX."""
        return (at["x"] * self.screen_width * POSITION_ONE // TABLET_RANGE,
                at["y"] * self.screen_height * POSITION_ONE // TABLET_RANGE)

    def pixel(self, at):
        """The pixel the pointer's place rounds down to, column and row."""
        return tuple(value // POSITION_ONE for value in self.place(at))

    def relative(self, at):
        """The place at in the window, in 256ths of a pixel, as a window is
        sent it: less its edges, so below 0 and past its size outside it
        (display.md D11, rule 6)."""
        across, down = self.place(at)
        return (across - self.left * POSITION_ONE,
                down - self.top * POSITION_ONE)

    def over_window(self, at):
        """Whether the pixel of the place at is one of the window's, those
        of its left and top edges in and those of its right and bottom
        edges out (display.md D11, rule 3)."""
        column, row = self.pixel(at)
        return (self.left <= column < self.left + self.width
                and self.top <= row < self.top + self.height)


def size_of(text):
    """The width and height in pixels of a WIDTHxHEIGHT key of a line."""
    width, height = text.split("x")
    return int(width), int(height)


def enter_event(geometry, at):
    """The client's line for its entry at the tablet's place at."""
    across, down = geometry.relative(at)
    return Event(ENTER_LINE, x_256ths=str(across), y_256ths=str(down),
                 time_ms=TIME_MS)


def pointer_event(geometry, at, wheel=0, buttons=()):
    """The client's line for a pointer event at the tablet's place at, the
    wheel turned by wheel detents and the buttons changed each a button's
    name, a colon and pressed or released."""
    across, down = geometry.relative(at)
    return Event(POINTER_LINE, x_256ths=str(across), y_256ths=str(down),
                 wheel_120ths=str(wheel * WHEEL_DETENT),
                 buttons=",".join(buttons) or "none", time_ms=TIME_MS)


LEAVE_EVENT = Event(LEAVE_LINE)
RELATIVE_LINE = Event("%s: pointer move mode=relative axes=xy" % COMPOSITOR)


class PointerStep:
    """What the boot sends and then waits for: events, each sent as one
    input-send-event; lines, the client's lines the window must be sent for
    them; wait, the Events to read before going on, the last of lines unless
    it is empty; screen, the tablet place whose pixel the screen must show
    the pointer at, or None; and hold, whether that screen must then hold
    still."""

    def __init__(self, events, lines, wait=None, screen=None, hold=False):
        self.events = events
        self.lines = lines
        self.wait = lines[-1:] if wait is None else wait
        self.screen = screen
        self.hold = hold


def pointer_steps(geometry):
    """The steps of the window boot's pointer, a model written from
    display.md D10 and D11 and not from the code it checks:

    - the tablet is placed away from the window: the pointer is drawn,
      over no window, and the window is sent nothing;
    - it moves onto the window: the window is sent its entry, and no
      pointer event, since the move that enters is its entry alone;
    - it moves about the window: a motion;
    - each button is clicked: a pointer event for the press and one for
      the release, at the place the pointer is at, with no motion;
    - the wheel turns up and down: a turn of one detent each way, in 120ths
      of a detent;
    - a button is pressed, the pointer moves out of the window and the
      button is released there: the window is sent the press, the motion,
      which is held to it and so past its edges, the release, and then its
      leave, which the last release puts after it and not as the pointer
      crosses the edge;
    - a button is pressed away from the window, the pointer moves onto the
      window and the button is released: the press is held to no window, so
      the window is sent no press and no release, and no pointer event but
      its entry, which the release, the last, makes;
    - it moves about the window again, and the mouse's relative motion
      follows, which moves no pointer and is sent no window: the screen
      must still show the pointer where the tablet put it, and hold."""
    steps = [
        PointerStep([move("abs", OUTSIDE)], [], screen=OUTSIDE),
        PointerStep([move("abs", ENTRY)], [enter_event(geometry, ENTRY)],
                    screen=ENTRY),
        PointerStep([move("abs", MOVED)], [pointer_event(geometry, MOVED)]),
    ]
    for name in BUTTONS:
        steps.append(PointerStep(
            [button(name, True), button(name, False)],
            [pointer_event(geometry, MOVED, buttons=[name + ":pressed"]),
             pointer_event(geometry, MOVED, buttons=[name + ":released"])]))
    steps += [
        PointerStep([button("wheel-up", True), button("wheel-up", False),
                     button("wheel-down", True), button("wheel-down", False)],
                    [pointer_event(geometry, MOVED, wheel=1),
                     pointer_event(geometry, MOVED, wheel=-1)]),
        PointerStep([move("abs", DRAG), button("left", True),
                     move("abs", BELOW), button("left", False)],
                    [pointer_event(geometry, DRAG),
                     pointer_event(geometry, DRAG, buttons=["left:pressed"]),
                     pointer_event(geometry, BELOW),
                     pointer_event(geometry, BELOW, buttons=["left:released"]),
                     LEAVE_EVENT]),
        PointerStep([button("left", True), move("abs", ENTRY),
                     button("left", False)],
                    [enter_event(geometry, ENTRY)]),
        PointerStep([move("abs", LAST)], [pointer_event(geometry, LAST)]),
        PointerStep([move("rel", RELATIVE_MOTION)], [], wait=[RELATIVE_LINE],
                    screen=LAST, hold=True),
    ]
    return steps


def check_pointer_places(geometry):
    """Fail unless the places are where pointer_steps wants them on
    geometry: OUTSIDE and BELOW outside the window, and the rest in it."""
    for name, at, inside in (("OUTSIDE", OUTSIDE, False),
                             ("ENTRY", ENTRY, True), ("MOVED", MOVED, True),
                             ("DRAG", DRAG, True), ("BELOW", BELOW, False),
                             ("LAST", LAST, True)):
        if geometry.over_window(at) != inside:
            raise BootFailed("the pointer's place %s is %s the %dx%d window "
                             "at %d,%d on a %dx%d screen, not %s it"
                             % (name, "outside" if inside else "inside",
                                geometry.width, geometry.height,
                                geometry.left, geometry.top,
                                geometry.screen_width, geometry.screen_height,
                                "inside" if inside else "outside"))
    if len({geometry.pixel(at) for at in POINTER_PLACES}) != len(
            POINTER_PLACES):
        raise BootFailed("two of the pointer's places are one pixel on a "
                         "%dx%d screen" % (geometry.screen_width,
                                           geometry.screen_height))


class PointerScript:
    """The lines the window boot's pointer must make (pointer_steps),
    checked as the Serial reads them: the test client's enter, pointer and
    leave lines, each compared with the next of the steps' lines, so that
    a line that is not that one fails the boot at it, and finish fails it
    if fewer were read than the steps give; and the compositor's pointer
    lines, of which finish checks that each of pointer_lines's was logged.
    Only the lines read before the boot ends are checked, so a client line
    past the last fails the boot only if it was read. finish also checks
    that the times of the client's lines do not fall from one to the next.
    It checks no other time."""

    def __init__(self, steps):
        self.events = [line for step in steps for line in step.lines]
        self.client = []
        self.compositor = []

    def take(self, parsed):
        """Check a line, parsed, of the boot."""
        if (parsed[0] == "nova-wintest"
                and parsed[1] in CLIENT_POINTER_EVENTS):
            self.take_client(parsed)
        elif parsed[0] == COMPOSITOR and parsed[1].startswith("pointer "):
            self.compositor.append(parsed)

    def take_client(self, parsed):
        index = len(self.client)
        if index >= len(self.events):
            raise BootFailed("a pointer line after the last that was sent: "
                             "%s" % describe(parsed))
        if not self.events[index].matches(parsed):
            raise BootFailed("pointer line %d is %s; the window should be "
                             "sent %s" % (index + 1, describe(parsed),
                                          describe_event(self.events[index])))
        self.client.append(parsed)

    def finish(self):
        """Check that every line has been read, and the times."""
        if len(self.client) != len(self.events):
            raise BootFailed("%d of %d pointer lines were read"
                             % (len(self.client), len(self.events)))
        times = [float(parsed[2]["time_ms"]) for parsed in self.client
                 if "time_ms" in parsed[2]]
        for earlier, later in zip(times, times[1:]):
            if later < earlier:
                raise BootFailed("a pointer line's time, %.1f ms, is before "
                                 "the one before it, %.1f ms"
                                 % (later, earlier))
        for event in pointer_lines(COMPOSITOR):
            if not any(event.matches(parsed) for parsed in self.compositor):
                raise BootFailed("the compositor did not log %s"
                                 % describe_event(event))


# What no line of the log may hold: the password, each word of the names
# and lines typed for the file commands, words no other line of the log
# holds, so that a log of any of them is found, but for the wheel's
# lines, single characters, which other lines hold, and, in a line of the
# console, an input driver or the compositor's pointer lines, the only
# processes that see it, the pointer's position as sent, which the driver
# passes on as it is, as a whole number. Other lines hold numbers that
# change from run to run, such as the kernel's timings and the
# generator's first bytes, which can hold a position's digits.
def literal(word):
    return re.compile(re.escape(word.encode("ascii")))


SEES_POSITION = (rb"(?:nova-(?:console|virtio-input): "
                 rb"|nova-compositor: pointer ).*")


def whole_number(value):
    return re.compile(SEES_POSITION + rb"(?<![0-9])%d(?![0-9])" % value)


UNLOGGED = ([(literal(ACCOUNT_PASSWORD), "the account's password")]
            + [(literal(word), "%r, typed for a file command" % word)
               for word in ["notes", "draft", "final", "stale", "nobody",
                            "alphabet"]
               + " ".join([FIRST_DRAFT] + FINAL_LINES).split()]
            + [(whole_number(value), "the pointer's %s" % axis)
               for place in (POINTER_AT, WHEEL_AT)
               for axis, value in place.items()])

# The window boot's pointer places, which the compositor must not log
# either, and the client logs only relative to its window.
WINDOW_UNLOGGED = [(whole_number(value), "the pointer's %s, %d" % (axis, value))
                   for at in POINTER_PLACES for axis, value in at.items()]

# The accounts service hashes each password with Argon2id over 64 MiB in
# 3 passes, which takes about 12 seconds under TCG on an Apple M2 and
# can take several times that on a shared runner; a line typed and
# answered without a hash takes a second or two.
HASH_TIMEOUT = 300
KEY_TIMEOUT = 60

# The logout disk's session program, test/driver.c, stands in for the
# shell. It runs CHURN_RUNS programs that end at once, one after
# another, which are more than the handles of login's table, so that a
# handle kept for each of them runs login out; then programs that stay
# running, each logging a beat every 100 ms (test/victim.c), until login
# refuses one. Login allows a session SESSION_PROGRAMS at once, the
# store service's clients less its control channel's and its root's
# (login/login.c, SESSION_PROGRAMS_MAX). The driver then ends, and login
# must stop every victim as it ends the session. A victim that beats
# again within QUIET_SECONDS, twenty times the beats that show one
# running, was not stopped. The boot then logs in again and requires the
# same of a second session.
CHURN_RUNS = 260
SESSION_PROGRAMS = 6
ERR_NO_RESOURCES = 15
QUIET_SECONDS = 2
CHURN_TIMEOUT = 600
VICTIMS_TIMEOUT = 120
HOLD_TIMEOUT = 60
DRIVER_SESSION = Event("nova-driver: session name=%s" % ACCOUNT_NAME)
CHURNED = Event("nova-driver: churned runs=%d" % CHURN_RUNS)
VICTIM_RUNS = (
    [Event("nova-driver: run id=%d status=%d" % (n, OK))
     for n in range(SESSION_PROGRAMS)]
    + [Event("nova-driver: run id=%d status=%d"
             % (SESSION_PROGRAMS, ERR_NO_RESOURCES)),
       Event('nova-login: program start refused reason="session limit" '
             "limit=%d" % SESSION_PROGRAMS)])
VICTIM_BEATS = [Event("nova-victim: beat id=%d" % n)
                for n in range(SESSION_PROGRAMS)]
ANY_BEAT = Event("nova-victim: beat")

# The lines that report a failure (docs/spec/log.md T1): an event that
# ends with FAILED, and these.
FAILED = "failed"
FAILURES = {("nova", "halted"), ("nova", "exception"),
            ("nova", "user exception"), ("nova", "first process ended"),
            ("nova", "random unseeded"), ("nova-init", "child restarted"),
            ("nova-init", "child given up")}


def reports_failure(parsed):
    component, event, _ = parsed
    return (event.split()[-1] == FAILED
            or (component, event) in FAILURES)


# What the fallback boot accepts of those lines, each with whether it
# comes once for each input function, told apart by its pci key, or once
# in all: the compositor restarted once and then given up, and each
# input driver started again without a restart counted, once for the
# compositor's restart and once for the console taking the clients' place
# (docs/spec/display.md D19, rule 9).
ACCEPTED_FAILURES = [(COMPOSITOR_RESTARTED, False),
                     (COMPOSITOR_GIVEN_UP, False)] + [
    (event, True) for event in INPUT_RESTARTED]

# What the no-answer boot accepts of those lines: the fallback boot's, and
# the first process's line for its wait that failed, once for each start
# of the compositor (docs/spec/display.md D19, rule 2, step 4).
NO_ANSWER_FAILURES = ACCEPTED_FAILURES + [
    (CONNECTION_UNANSWERED.repeated(COMPOSITOR_STARTS), False)]


def accepted_failures(mode):
    """The failure lines a boot in mode accepts: those of the fallback
    boot in fallback mode, those of the no-answer boot in noanswer mode,
    and none in any other."""
    return {"fallback": ACCEPTED_FAILURES,
            "noanswer": NO_ANSWER_FAILURES}.get(mode, [])


def refused_lines(mode):
    """The lines, besides those that report a failure, that fail a boot
    in mode: the window boot's WINDOW_REFUSED, the console in a window's
    COMPOSITOR_REFUSED, the no-answer boot's NO_ANSWER_REFUSED, and none
    in any other."""
    if mode == "window":
        return WINDOW_REFUSED
    if mode == "noanswer":
        return NO_ANSWER_REFUSED
    return COMPOSITOR_REFUSED if mode in WINDOW_PATH_MODES else []


class Accepted:
    """The failure lines a boot may report, as ACCEPTED_FAILURES gives
    them, and none other: take a line, and it is accepted only as often
    as its Event is to be reported, once unless it says more, in all or
    for its input function, of which there are INPUT_FUNCTIONS at most,
    each with a pci key."""

    def __init__(self, accepted):
        self.accepted = accepted
        self.seen = {}
        self.functions = set()

    def take(self, parsed):
        for index, (event, per_function) in enumerate(self.accepted):
            if not event.matches(parsed):
                continue
            function = parsed[2].get("pci") if per_function else None
            if per_function and (function is None
                                 or (function not in self.functions
                                     and len(self.functions)
                                     == INPUT_FUNCTIONS)):
                return False
            key = (index, function)
            if self.seen.get(key, 0) == event.times:
                return False
            self.seen[key] = self.seen.get(key, 0) + 1
            if per_function:
                self.functions.add(function)
            return True
        return False


# A test report in KTAP version 1 (docs/spec/log.md T2, as
# docs/spec/README.md pins it): a version line, a plan, then a result
# for each test, a failure's diagnostics before it, and a test's own
# tests, a whole report of their own one KTAP_INDENT further in, just
# before its result. A line that is none of these is an unknown line,
# which KTAP allows anywhere.
KTAP_VERSION = "KTAP version 1"
KTAP_INDENT = "  "
KTAP_PLAN = re.compile(r"1\.\.(\d+)")
KTAP_RESULT = re.compile(r"(ok|not ok) (\d+)(?: ([^#]*?))?(?: # (.*))?")
KTAP_DIAGNOSTIC = "#"
KTAP_SKIP = re.compile(r"SKIP(?: (.*))?")


class KtapList:
    """The tests at one depth of a report: the plan, once read, and each
    test's result as it comes."""

    def __init__(self, depth):
        self.depth = depth
        self.plan = None
        self.tests = []


class KtapTest:
    """A test's result: its name, whether it passed, the reason it was
    skipped, or None if it ran, the data after its hash sign if it ran,
    the diagnostics written at its depth since the result before it, the
    list of its own tests, or None, and whether its result line came, or
    the test stands for a list whose result never did."""

    def __init__(self, name, passed, skipped, data, diagnostics, tests,
                 reported=True):
        self.name = name
        self.passed = passed
        self.skipped = skipped
        self.data = data
        self.diagnostics = diagnostics
        self.tests = tests
        self.reported = reported


def reported(tests):
    """How many of tests' results came."""
    return sum(1 for test in tests.tests if test.reported)


class Ktap:
    """A boot's test report, read a line at a time. done once the top
    plan's last result has come; errors notes each line out of place."""

    def __init__(self):
        self.lists = []
        self.top = None
        self.diagnostics = {}
        self.errors = []
        self.done = False

    def feed(self, line):
        text = line.rstrip()
        indent = len(text) - len(text.lstrip(" "))
        text = text.strip()
        if self.done or not text or indent % len(KTAP_INDENT):
            return
        depth = indent // len(KTAP_INDENT)
        if text == KTAP_VERSION:
            self.version(depth)
            return
        plan = KTAP_PLAN.fullmatch(text)
        result = KTAP_RESULT.fullmatch(text)
        if depth >= len(self.lists):
            if self.top is not None and (plan or result):
                self.errors.append("a %s at depth %d with no version line "
                                   "before it" % ("plan" if plan
                                                  else "result", depth))
            return
        if plan and depth == len(self.lists) - 1:
            self.set_plan(self.lists[depth], int(plan.group(1)))
        elif result:
            self.result(depth, result)
        elif text.startswith(KTAP_DIAGNOSTIC):
            self.diagnostics.setdefault(depth, []).append(
                text[len(KTAP_DIAGNOSTIC):].strip())

    def version(self, depth):
        if depth == 0 and self.top is None:
            self.top = KtapList(0)
            self.lists = [self.top]
        elif depth == len(self.lists) and depth > 0:
            self.lists.append(KtapList(depth))
        else:
            self.errors.append("a version line at depth %d out of place"
                               % depth)

    def set_plan(self, tests, count):
        if tests.plan is not None or tests.tests:
            self.errors.append("a plan at depth %d after its first"
                               % tests.depth)
        else:
            tests.plan = count

    def close(self, tests, name):
        """Check that tests, the list of the test name, holds its plan."""
        if tests.plan is None:
            self.errors.append("%s: no plan" % name)
        elif len(tests.tests) != tests.plan:
            self.errors.append("%s: %d results for its plan of 1..%d"
                               % (name, len(tests.tests), tests.plan))

    def result(self, depth, match):
        """A result at depth, which ends a deeper list if one is open:
        that list is this test's own."""
        number, name = int(match.group(2)), (match.group(3) or "").strip()
        own = None
        while len(self.lists) > depth + 1:
            deeper = self.lists.pop()
            if len(self.lists) == depth + 1:
                own = deeper
            else:
                self.close(deeper, "a list at depth %d" % deeper.depth)
        if own:
            self.close(own, name)
        tests = self.lists[depth]
        if tests.plan is None:
            self.errors.append("%s: a result at depth %d before a plan"
                               % (name, depth))
        if number != len(tests.tests) + 1:
            self.errors.append("%s: numbered %d, not %d"
                               % (name, number, len(tests.tests) + 1))
        data = match.group(4)
        skip = KTAP_SKIP.fullmatch(data or "")
        tests.tests.append(KtapTest(
            name, match.group(1) == "ok",
            (skip.group(1) or "") if skip else None,
            None if skip else data, self.diagnostics.pop(depth, []), own))
        for deeper in [d for d in self.diagnostics if d > depth]:
            del self.diagnostics[deeper]
        if depth == 0 and tests.plan == len(tests.tests):
            self.done = True

    def finish(self):
        """Check every list still open, once no more lines will come. An
        open list below the top is the list of a test whose result never
        came, which is added to the list above it as not ok, so that its
        tests are reported."""
        unfinished = []
        for depth in range(len(self.lists) - 1, -1, -1):
            tests = self.lists[depth]
            if depth == 0:
                self.close(tests, "the report")
                continue
            above = self.lists[depth - 1]
            name = "<test %d, no result>" % (len(above.tests) + 1)
            self.close(tests, name)
            unfinished.append((above, name, tests))
        for above, name, tests in reversed(unfinished):
            above.tests.append(KtapTest(name, False, None, None, [], tests,
                                        reported=False))
        self.lists = []

    def walk(self, tests=None, prefix=""):
        """Each test, below tests or the top, with its path of names."""
        for test in (tests or self.top).tests:
            path = prefix + test.name
            yield path, test
            if test.tests:
                yield from self.walk(test.tests, path + "/")

    def problems(self, required, plans, skipped, data):
        """What fails the report: no version line, a line out of place, a
        test not ok, a test of required missing, failed or skipped, a
        list of plans' paths whose plan is not the count given, a test of
        skipped that ran, or a test of data's paths whose result does not
        carry the data given. A report that stopped short names no
        missing test: its count does."""
        if self.top is None:
            return ["no test report: no %r line" % KTAP_VERSION]
        found = dict(self.walk())
        problems = list(self.errors)
        problems += ["%s: not ok" % path for path, test in found.items()
                     if not test.passed]
        for path in required:
            test = found.get(path)
            if test is None:
                if self.done:
                    problems.append("%s: not reported" % path)
            elif test.skipped is not None:
                problems.append("%s: skipped, %s" % (path, test.skipped))
        for path, count in plans.items():
            test = found.get(path)
            if test and test.skipped is None and (
                    not test.tests or test.tests.plan != count):
                problems.append("%s: not %d tests" % (path, count))
        for path in skipped:
            test = found.get(path)
            if test and test.skipped is None:
                problems.append("%s: ran, where it must be skipped" % path)
        for path, wanted in data.items():
            test = found.get(path)
            if test and test.passed and test.data != wanted:
                problems.append("%s: carries %r, not %r"
                                % (path, test.data, wanted))
        return problems

    def summary(self):
        """The report as the check prints it: the plan, each test with its
        own plan, and every test not ok or skipped with its
        diagnostics or reason."""
        if self.top is None:
            return ["ktap: no report"]
        lines = ["ktap: plan 1..%s, %d reported"
                 % (self.top.plan, reported(self.top))]
        for path, test in self.walk():
            plan = (" (1..%s, %d reported)"
                    % (test.tests.plan, reported(test.tests))
                    if test.tests else "")
            if not test.passed:
                lines.append("ktap: not ok %s%s" % (path, plan))
                lines += ["ktap:   # " + line for line in test.diagnostics]
            elif test.skipped is not None:
                lines.append("ktap: skipped %s: %s" % (path, test.skipped))
            elif test.tests:
                lines.append("ktap: ok %s%s" % (path, plan))
        return lines


QMP_TIMEOUT = 30
QUIT_TIMEOUT = 30

# The data disk's writable copy, in the scratch directory.
DATA_COPY = "data.img"


class BootFailed(Exception):
    pass


def check_spinner(keys, line):
    """Fail if keys, those of the kernel's background line, show fewer
    than MIN_SPINNER_FRAMES over a preparation that may have lasted
    longer than SPINNER_GAP_MS; say so if they show fewer over one that
    ended within it. The logged time is rounded down, so the preparation
    ended within the bound only if a tenth of a millisecond more does."""
    frames = int(keys["spinner_frames"])
    whole, tenth = (int(part) for part in keys["prepare_ms"].split("."))
    if frames >= MIN_SPINNER_FRAMES:
        return
    if (whole * TENTHS_PER_MS + tenth + 1
            > SPINNER_GAP_MS * TENTHS_PER_MS):
        raise BootFailed("the spinner stood still: " + line)
    print("          the background was prepared within the %d ms a "
          "turning spinner can go between frames" % SPINNER_GAP_MS,
          flush=True)


def check_display(keys, line, size):
    """Fail if keys, those of a display line, the console's or the
    compositor's, name a screen other than size, the width and height
    QEMU was asked for."""
    asked = "%dx%d" % size
    if keys["screen"] != asked:
        raise BootFailed("the display is %s, not the %s the boot asked "
                         "for: %s" % (keys["screen"], asked, line))


class Serial:
    """The guest's serial lines, read from QEMU's standard output. What
    a line holds of UNLOGGED is noted in leaked. With report, a Ktap,
    every line is read into it; without, a report's version line fails
    the boot. With size, a display line that names another size fails
    it. A line that reports a failure fails the boot unless the mode's
    accepted_failures take it, and a line that matches one of the mode's
    refused_lines fails it. Each of watches, pairs of an Event and a
    function of no arguments, calls the function once, when the first
    line that matches the Event is read. While script, a KeyScript or a
    PointerScript, is set, each line is checked against it as it is read.
    The keys of the window line of the test client or of the console, once
    it is read, are kept in window, and the window boot's pointer places
    are among the lines' unlogged words. In the fallback boot, the console
    makes its window before each end of the compositor. The keys of each
    budget line of the compositor are kept in budgets, under its kind, in
    the order read; one that is not in the form display.md D19's rule 8
    gives fails the boot, and so does any without report, since only a
    test disk's compositor writes them."""

    def __init__(self, stream, log, report, size, mode, watches=()):
        self.script = None
        self.window = None
        self.budgets = {BUDGET_POINTER: [], BUDGET_PRESENT: []}
        self.mode = mode
        self.windows = 0
        self.exits = 0
        self.lines = queue.Queue()
        self.start = time.monotonic()
        self.leaked = set()
        self.unlogged = UNLOGGED + (WINDOW_UNLOGGED if mode == "window"
                                    else [])
        self.report = report
        self.size = size
        self.accepted = Accepted(accepted_failures(mode))
        self.refused = refused_lines(mode)
        self.watches = list(watches)
        self.reader = threading.Thread(target=self.read, args=(stream, log))
        self.reader.start()

    def read(self, stream, log):
        for raw in stream:
            log.write(raw)
            log.flush()
            text = raw.rstrip(b"\r\n").replace(b"\r", b"")
            self.leaked.update(what for unlogged, what in self.unlogged
                               if unlogged.search(text))
            self.lines.put(text.decode("ascii", "replace"))
        self.lines.put(None)

    def wait_for(self, wanted, deadline, report_done=False):
        """Read lines until each Event in wanted has matched as many
        lines as it must be reported, and with report_done, until the
        test report has ended."""
        seen = [0] * len(wanted)
        while self.unseen(wanted, seen) or (report_done
                                            and not self.report.done):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise BootFailed("timed out; not reported:\n  "
                                 + self.unreported(wanted, seen,
                                                   report_done))
            try:
                line = self.lines.get(timeout=remaining)
            except queue.Empty:
                continue
            if line is None:
                raise BootFailed("QEMU ended; not reported:\n  "
                                 + self.unreported(wanted, seen,
                                                   report_done))
            parsed = self.take(line)
            for index, event in enumerate(wanted):
                if event.matches(parsed):
                    seen[index] += 1

    def read_for(self, seconds):
        """Read the lines that have come, and those that come within
        seconds, checking each as wait_for does."""
        deadline = time.monotonic() + seconds
        while True:
            try:
                line = self.lines.get(
                    timeout=max(deadline - time.monotonic(), 0))
            except queue.Empty:
                return
            if line is None:
                raise BootFailed("QEMU ended")
            self.take(line)

    def take(self, line):
        """Print a line read from the guest and check it, as the class
        says; its component, event and keys, or None for a line that is
        not nova-os's."""
        print("%8.1f  %s" % (time.monotonic() - self.start, line),
              flush=True)
        if self.report:
            self.report.feed(line)
        elif line.strip() == KTAP_VERSION:
            raise BootFailed("a test report from a disk that is not a "
                             "test disk: " + line)
        parsed = parse_line(line)
        if not parsed:
            if LINE_START.search(line):
                raise BootFailed("a line not in the log's form "
                                 "(docs/spec/log.md T1): " + line)
            return None
        if reports_failure(parsed) and not self.accepted.take(parsed):
            raise BootFailed("reported a failure: "
                             + line[LINE_START.search(line).start():])
        if any(event.matches(parsed) for event in self.refused):
            raise BootFailed("reported a line this boot refuses: "
                             + line[LINE_START.search(line).start():])
        if self.script:
            self.script.take(parsed)
        if WINDOW_MADE.matches(parsed) or CONSOLE_WINDOW.matches(parsed):
            self.window = parsed[2]
        if parsed[:2] == BUDGET_LINE:
            self.take_budget(line)
        if self.mode == "fallback":
            self.check_window_before_end(parsed, line)
        if BACKGROUND.matches(parsed):
            check_spinner(parsed[2], line)
        if self.size and any(d.matches(parsed) for d in DISPLAYS):
            check_display(parsed[2], line, self.size)
        for watch in [w for w in self.watches if w[0].matches(parsed)]:
            self.watches.remove(watch)
            watch[1]()
        return parsed

    def take_budget(self, line):
        """Keep the keys of line, a budget line of the compositor, under
        the line's kind. Fail if the disk is not a test disk, whose
        compositor alone writes one, or if the text after the line's
        event is not, whole, what display.md D19's rule 8 gives
        (BUDGET_FORM), which a key lost, added, repeated, quoted or out
        of order is not."""
        text = line[LINE_START.search(line).start():]
        if not self.report:
            raise BootFailed("a budget line from a disk that is not a test "
                             "disk: " + text)
        form = BUDGET_FORM.fullmatch(find_line(line).group(3))
        if not form:
            raise BootFailed("a budget line that is not in the form "
                             "docs/spec/display.md D19's rule 8 gives: "
                             + text)
        keys = form.groupdict()
        self.budgets[keys["change"]].append(keys)

    def check_window_before_end(self, parsed, line):
        """Fail if parsed, a line of the fallback boot, is an end of the
        compositor that the console's window did not come before: each end
        is of a compositor that ends after the first frame that shows a
        window's present (docs/spec/display.md D19, rule 9)."""
        if CONSOLE_WINDOW.matches(parsed):
            self.windows += 1
        elif COMPOSITOR_ENDED.matches(parsed):
            self.exits += 1
            if self.windows < self.exits:
                raise BootFailed("the compositor ended before the console "
                                 "made its window: "
                                 + line[LINE_START.search(line).start():])

    def wait_quiet(self, seconds, forbidden):
        """Read lines for seconds, failing at the first that matches the
        Event forbidden or that reports a failure."""
        deadline = time.monotonic() + seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            try:
                line = self.lines.get(timeout=remaining)
            except queue.Empty:
                return
            if line is None:
                raise BootFailed("QEMU ended before the quiet did")
            print("%8.1f  %s" % (time.monotonic() - self.start, line),
                  flush=True)
            parsed = parse_line(line)
            if parsed and forbidden.matches(parsed):
                raise BootFailed("reported after it was to have stopped: "
                                 + line[LINE_START.search(line).start():])
            if parsed and reports_failure(parsed):
                raise BootFailed("reported a failure: "
                                 + line[LINE_START.search(line).start():])

    @staticmethod
    def unseen(wanted, seen):
        """The indexes of the events in wanted that lines have matched
        fewer times than they must be reported, seen."""
        return [index for index, event in enumerate(wanted)
                if seen[index] < event.times]

    def unreported(self, wanted, seen, report_done):
        return "\n  ".join(
            [str(wanted[index]) + (" (%d reported)" % seen[index]
                                   if wanted[index].times > 1 else "")
             for index in self.unseen(wanted, seen)]
            + (["the end of the test report"]
               if report_done and not self.report.done else []))


class Qmp:
    """QMP over a stream connected to QEMU's monitor socket."""

    def __init__(self, stream):
        self.stream = stream
        self.receive()
        self.execute("qmp_capabilities")

    def receive(self):
        line = self.stream.readline()
        if not line:
            raise BootFailed("QMP connection closed")
        return json.loads(line)

    def execute(self, command, **arguments):
        message = {"execute": command}
        if arguments:
            message["arguments"] = arguments
        self.stream.write(json.dumps(message).encode("ascii") + b"\n")
        self.stream.flush()
        while True:
            reply = self.receive()
            if "error" in reply:
                raise BootFailed("QMP %s: %s" % (command,
                                                 reply["error"]["desc"]))
            if "return" in reply:
                return reply["return"]


def type_line(qmp, text, shown=True):
    """Type text and Enter; a password is not printed."""
    print("          send-key %s, Enter" % (text if shown else "(password)"),
          flush=True)
    for name in [QCODES.get(c, c) for c in text] + ["ret"]:
        qmp.execute("send-key", keys=[{"type": "qcode", "data": name}])


def send_events(qmp, events):
    """Send events, one QMP input-send-event, which QEMU ends with one
    SYN_REPORT, and print them."""
    print("          input-send-event %s"
          % ", ".join("%s %s" % (e["type"], " ".join(
              str(v) for v in e["data"].values())) for e in events),
          flush=True)
    qmp.execute("input-send-event", events=events)


def point(serial, qmp, window):
    """Send the pointer's events and wait for the lines for them: the
    compositor's where the console is in a window, as window, that
    window's line's keys, says, and the console's where it has the
    pointing device."""
    for events in POINTER_EVENTS:
        send_events(qmp, events)
    serial.wait_for(COMPOSITOR_POINTER_LINES if window else POINTER_LINES,
                    time.monotonic() + KEY_TIMEOUT)


def run_commands(serial, qmp, commands, data):
    """Type each command's lines, wait for the status it must log, and
    check the home's tree on data, the data disk, which QEMU holds open
    but writes no more until the next line is typed."""
    for lines, command, status, wanted in commands:
        for text in lines:
            type_line(qmp, text)
        serial.wait_for([Event(STATUS_RAN % (command, status))],
                        time.monotonic() + KEY_TIMEOUT)
        check_home(data, wanted)
        print("          the data disk holds the home %s left" % command,
              flush=True)


def sign_in(serial, qmp, first, session):
    """Create the account on a first boot, typing its password twice, or
    log in with it on a later one, and wait for the line session, which
    its session's program logs."""
    type_line(qmp, ACCOUNT_NAME)
    type_line(qmp, ACCOUNT_PASSWORD, shown=False)
    if first:
        type_line(qmp, ACCOUNT_PASSWORD, shown=False)
    serial.wait_for([CREATED if first else LOGGED_IN, session],
                    time.monotonic() + HASH_TIMEOUT)


def log_in(serial, qmp, data, first):
    """Sign in as sign_in does, then run whoami and the file commands in
    the shell, checking data, the data disk, after each."""
    sign_in(serial, qmp, first, SESSION)
    type_line(qmp, WHOAMI)
    serial.wait_for([WHOAMI_RAN], time.monotonic() + KEY_TIMEOUT)
    type_line(qmp, ACCOUNT_PASSWORD, shown=False)
    serial.wait_for([NO_COMMAND], time.monotonic() + KEY_TIMEOUT)
    run_commands(serial, qmp,
                 FILE_COMMANDS if first else LATER_FILE_COMMANDS, data)


def log_out(serial, qmp):
    """End the session with logout and wait for login to report it
    ended."""
    type_line(qmp, LOGOUT)
    serial.wait_for([session_ended(0)], time.monotonic() + KEY_TIMEOUT)


def check_budgets(serial, qmp, screen):
    """Have the compositor of a test disk write its budget lines
    (docs/spec/display.md D19, rule 8), and print them: with the shell at
    its prompt in the console's window, send each of BUDGET_MOVES in turn
    until BUDGET_LINES lines for the pointer have been read since the
    first move, then each of BUDGET_TYPED in turn until as many for a
    present have been read since the first key, reading the log for
    BUDGET_PACE after each, and print the last of each kind, the line
    whose frames the boot's own moves or keys made, with screen, the
    screen's size as the console's window line gives it. A letter is
    always sent with the Backspace that erases it, so the prompt is left
    empty. Fail if SCREEN_TIMEOUT passes before the lines have come. A
    figure over display.md D9's budget fails nothing: a shared runner's
    time varies, and the figures are printed to be read.

    The lines read before a kind's first move or key are not counted,
    and every line of the kind that the compositor wrote before then has
    been read by then: it composes a frame of the pointer's only when the
    boot moves the tablet, which the boot last did before it logged in or
    turned the wheel, and no window presents while the tablet alone
    moves, which takes far longer than a line takes to arrive."""
    changes = [
        (BUDGET_POINTER, [move("abs", at) for at in BUDGET_MOVES],
         " and ".join("abs %d %d" % (at["x"], at["y"])
                      for at in BUDGET_MOVES)),
        (BUDGET_PRESENT, [[key_event(item) for item in keys.split()]
                          for keys in BUDGET_TYPED],
         " and ".join("keys " + keys for keys in BUDGET_TYPED))]
    deadline = time.monotonic() + SCREEN_TIMEOUT
    serial.read_for(0)
    for change, sends, sent in changes:
        lines = serial.budgets[change]
        wanted = len(lines) + BUDGET_LINES
        print("          input-send-event %s, in turn, until %d budget "
              "lines for change=%s" % (sent, BUDGET_LINES, change),
              flush=True)
        while len(lines) < wanted:
            if time.monotonic() >= deadline:
                raise BootFailed(
                    "after %d s the compositor has logged %d of the %d "
                    "budget lines for change=%s that the boot's moves and "
                    "keys must make" % (SCREEN_TIMEOUT,
                                        len(lines) + BUDGET_LINES - wanted,
                                        BUDGET_LINES, change))
            for events in sends:
                qmp.execute("input-send-event", events=events)
                serial.read_for(BUDGET_PACE)
        print("          the %s screen's budget for change=%s: %s"
              % (screen, change, " ".join(
                  "%s=%s" % item for item in lines[wanted - 1].items()
                  if item[0] != "change")), flush=True)


def drive(serial, qmp_path, data, accounts, first, screens, scratch,
          tests, window):
    """Over QMP: send the pointer's events, then, with accounts, and with
    screens, the program ci/screens.c builds to, check the login prompt
    as check_prompt does, create the account or log in with it as log_in
    does, and with screens check the wheel as scroll does, with the caret
    steady on a test disk, which tests says the disk is, and not on
    another; then, with accounts, on a test disk whose console is in a
    window, have the compositor write its budget lines as check_budgets
    does, and end the session as log_out does. window is the keys of the
    console's window line where the console is in a window, or None."""
    geometry = Geometry.of(window) if window else None
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(QMP_TIMEOUT)
        connection.connect(qmp_path)
        with connection.makefile("rwb") as stream:
            qmp = Qmp(stream)
            point(serial, qmp, window)
            if accounts and screens:
                check_prompt(qmp, scratch, screens, tests, geometry, first)
            if accounts:
                log_in(serial, qmp, data, first)
            if screens:
                scroll(serial, qmp, data, screens, scratch, tests,
                       geometry)
            if accounts and tests and window:
                check_budgets(serial, qmp, window["screen"])
            if accounts:
                log_out(serial, qmp)


def check_programs(serial):
    """What the logout disk's driver and login must report of a session,
    once its driver has started: the churn, the victims refused at login's
    limit and each beating, the session ended with every victim stopped,
    and no victim heard from after. Each wait takes the lines it reads
    off the log, and the last reads to the end of its quiet, so that the
    lines of the next session are the only ones a later call sees."""
    serial.wait_for([CHURNED], time.monotonic() + CHURN_TIMEOUT)
    serial.wait_for(VICTIM_RUNS + VICTIM_BEATS,
                    time.monotonic() + VICTIMS_TIMEOUT)
    serial.wait_for([session_ended(SESSION_PROGRAMS)],
                    time.monotonic() + HOLD_TIMEOUT)
    serial.wait_quiet(QUIET_SECONDS, ANY_BEAT)


def drive_logout(serial, qmp_path):
    """Over QMP, create the account, which starts the session's driver,
    and check_programs; then, with login back at its prompt, log in with
    the account, which starts a second driver, and check_programs again,
    which the second session passes only if the first left login's
    places for programs free, as it left the store service's clients.
    Its driver must start afresh: sign_in waits for the line the driver
    logs as it starts, which the first session's already read."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(QMP_TIMEOUT)
        connection.connect(qmp_path)
        with connection.makefile("rwb") as stream:
            qmp = Qmp(stream)
            for first in (True, False):
                sign_in(serial, qmp, first, DRIVER_SESSION)
                check_programs(serial)


def read_ppm(path):
    """The width, height and pixel bytes of a binary PPM of maxval 255,
    with no comment: the magic, width, height and maxval separated by
    whitespace, then one whitespace byte, then the pixels (Netpbm's PPM
    format)."""
    with open(path, "rb") as f:
        data = f.read()
    fields, at = [], 0
    for _ in range(PPM_FIELDS):
        while at < len(data) and data[at:at + 1].isspace():
            at += 1
        start = at
        while at < len(data) and not data[at:at + 1].isspace():
            at += 1
        fields.append(data[start:at])
    if (fields[0] != PPM_MAGIC or not fields[1].isdigit()
            or not fields[2].isdigit() or fields[3] != PPM_MAXVAL):
        raise BootFailed("%s is not a binary PPM of maxval 255" % path)
    width, height = int(fields[1]), int(fields[2])
    pixels = data[at + 1:]
    if len(pixels) != 3 * width * height:
        raise BootFailed("%s holds %d pixel bytes, not %d"
                         % (path, len(pixels), 3 * width * height))
    return width, height, pixels


def check_specimen(qmp_path, scratch, fonts, screen):
    """Take QMP's screendump, keep it at screen if given, and compare it
    with the prototype's specimen at its size."""
    shot = screen or os.path.join(scratch, SCREEN_COPY)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(QMP_TIMEOUT)
        connection.connect(qmp_path)
        with connection.makefile("rwb") as stream:
            Qmp(stream).execute("screendump",
                                filename=os.path.abspath(shot))
    width, height, pixels = read_ppm(shot)
    wanted_path = os.path.join(scratch, PROTOTYPE_COPY)
    subprocess.run([sys.executable, "-I", SPECIMEN_PROTOTYPE, fonts,
                    str(width), str(height), wanted_path], check=True)
    _, _, wanted = read_ppm(wanted_path)
    differ = differing(pixels, wanted)
    if differ:
        raise BootFailed("the %dx%d screen differs from the specimen's "
                         "prototype in %d pixels" % (width, height, differ))
    print("          the %dx%d screen equals the specimen's prototype, "
          "sha256 %s" % (width, height, hashlib.sha256(pixels).hexdigest()),
          flush=True)


def differing(pixels, wanted):
    """How many pixels of two images of one size differ."""
    return sum(1 for i in range(0, len(pixels), 3)
               if pixels[i:i + 3] != wanted[i:i + 3])


def build_screens(scratch):
    """Build ci/screens.c in scratch; the program's path."""
    program = os.path.join(scratch, SCREENS_PROGRAM)
    command = (shlex.split(os.environ.get("CC") or "cc") + SCREENS_FLAGS
               + [flag for include in SCREENS_INCLUDES
                  for flag in ("-I", os.path.join(ROOT, include))]
               + ["-o", program]
               + [os.path.join(ROOT, source) for source in SCREENS_SOURCES])
    print(" ".join(command), flush=True)
    if subprocess.run(command).returncode != 0:
        raise BootFailed("ci/screens.c did not build")
    return program


def draw_screen(screens, scratch, kind, width, height, inputs):
    """The pixels of the screen of kind that screens draws at width x
    height from inputs."""
    path = os.path.join(scratch, EXPECTED_COPY)
    command = [screens, kind, str(width), str(height)] + inputs
    with open(path, "wb") as out:
        if subprocess.run(command, stdout=out).returncode != 0:
            raise BootFailed("ci/screens.c drew no screen")
    return read_ppm(path)[2]


# The screens the compositor shows from its first frame to its exit in the
# fallback boot, which the compositor's first display line comes after: its
# first frame, Limb alone, and the frame that shows the console's window,
# whose text is empty and which has no caret, since the console has been
# given nothing to write and no read waits (docs/spec/display.md D19,
# rule 9).
FIRST_FRAME_SCREENS = ("Limb alone", "the console's window over Limb")


def first_frame_screens(screens, scratch, width, height):
    """The pixels of each of FIRST_FRAME_SCREENS, in order, at width x
    height, as ci/screens.c draws them."""
    return [draw_screen(screens, scratch, LIMB_SCREEN, width, height, []),
            draw_screen(screens, scratch, CONSOLE_WINDOW_SCREEN, width,
                        height, [CARET, "off"])]


def match_first_frame(qmp, scratch, screens):
    """Take QMP's screendump until its pixels are one of
    FIRST_FRAME_SCREENS as ci/screens.c draws them, or SCREEN_TIMEOUT
    passes with a BootFailed that counts the pixels of the nearer one that
    differ. Returns the name of the one it was."""
    shot = os.path.abspath(os.path.join(scratch, SCREEN_COPY))
    wanted, _ = poll_screen(
        qmp, shot, lambda width, height: first_frame_screens(
            screens, scratch, width, height))
    return FIRST_FRAME_SCREENS[wanted.index(read_ppm(shot)[2])]


def check_limb(qmp_path, scratch, screens):
    """Fail unless the screen is, in some dump taken from when the
    compositor's first display line is read, one of the two frames the
    compositor shows before it ends: Limb alone, or the console's window
    over Limb (match_first_frame). The dumps are polled, so a dump taken
    while a frame is composed, which matches neither, is taken again.

    This shows that each frame the compositor shows is a whole one, the
    first Limb as the kernel's fade left it and the next the console's
    window composed over it, as ci/screens.c draws them by the code the
    compositor composes with and the console draws with. It does not show
    the order: the console's window may be shown before the first dump, in
    which case Limb alone is never seen. A frame the compositor does not
    write at all leaves the kernel's Limb, the same pixels, and is not
    seen. The boot's later screens, after the compositor is given up,
    are the console's own."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(QMP_TIMEOUT)
        connection.connect(qmp_path)
        with connection.makefile("rwb") as stream:
            shown = match_first_frame(Qmp(stream), scratch, screens)
    print("          the screen after the compositor's first display line is "
          "%s" % shown, flush=True)


def poll_screen(qmp, shot, draw):
    """Take QMP's screendump, which it writes to shot, until its pixels'
    SHA-256 is that of one of the images draw gives, a function of the
    screen's width and height, or SCREEN_TIMEOUT passes. Returns the
    images and their digests."""
    deadline = time.monotonic() + SCREEN_TIMEOUT
    qmp.execute("screendump", filename=shot)
    width, height, pixels = read_ppm(shot)
    wanted = draw(width, height)
    digests = [hashlib.sha256(image).digest() for image in wanted]
    while hashlib.sha256(pixels).digest() not in digests:
        if time.monotonic() >= deadline:
            raise BootFailed("after %d s the %dx%d screen differs from "
                             "ci/screens.c's in %d pixels"
                             % (SCREEN_TIMEOUT, width, height,
                                min(differing(pixels, image)
                                    for image in wanted)))
        qmp.execute("screendump", filename=shot)
        _, _, pixels = read_ppm(shot)
    print("          the %dx%d screen equals ci/screens.c's, sha256 %s"
          % (width, height, hashlib.sha256(pixels).hexdigest()), flush=True)
    return wanted, digests


def match_screen(qmp, scratch, screens, inputs, steady, geometry=None,
                 at=None):
    """Take QMP's screendump until its pixels' SHA-256 is that of the
    console's screen screens draws at its size from inputs, or
    SCREEN_TIMEOUT passes: the console in a window with the pointer at the
    pixel the tablet's place at gives on geometry's screen, where there is
    a geometry, and the console on its own where there is none. The caret
    is in either phase of its blink, which a screen with no caret in view
    draws alike; where it is steady, it is lit, and the screen must then
    stay that one for STEADY_SECONDS, since a blinking caret matches
    whenever it is lit."""
    shot = os.path.abspath(os.path.join(scratch, SCREEN_COPY))
    kind, pointer = CONSOLE_SCREEN, []
    if geometry:
        column, row = geometry.pixel(at)
        kind, pointer = CONSOLE_WINDOW_SCREEN, [POINTER_INPUT, str(column),
                                                str(row)]
    wanted, digests = poll_screen(
        qmp, shot,
        lambda width, height: [
            draw_screen(screens, scratch, kind, width, height,
                        inputs + [CARET, phase] + pointer)
            for phase in (CARET_STEADY if steady else CARET_PHASES)])
    if steady:
        hold_screen(qmp, shot, wanted, digests)


def check_window(qmp_path, scratch, screens):
    """Take QMP's screendump until it equals the screen ci/screens.c
    draws for the window test client's window at its size, or
    SCREEN_TIMEOUT passes. The client has logged its second frame, which
    the compositor has composed and flushed before it sends the frame's
    event, but QEMU shows the guest's memory at its own refresh."""
    shot = os.path.abspath(os.path.join(scratch, SCREEN_COPY))
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(QMP_TIMEOUT)
        connection.connect(qmp_path)
        with connection.makefile("rwb") as stream:
            poll_screen(Qmp(stream), shot,
                        lambda width, height: [
                            draw_screen(screens, scratch, WINDOW_SCREEN,
                                        width, height, [])])


def key_event(item):
    """The QMP key event of an item of WINDOW_KEYS: a key's name and a
    plus or a minus."""
    return {"type": "key", "data": {
        "down": item.endswith("+"),
        "key": {"type": "qcode", "data": item[:-1]}}}


def match_pointer(qmp, scratch, screens, geometry, at, hold):
    """Take QMP's screendump until it equals the window's screen
    ci/screens.c draws with the pointer at the pixel the tablet's place at
    gives on geometry's screen, or SCREEN_TIMEOUT passes; and, with hold,
    until it has held still for STEADY_SECONDS."""
    shot = os.path.abspath(os.path.join(scratch, SCREEN_COPY))
    column, row = geometry.pixel(at)
    wanted, digests = poll_screen(
        qmp, shot,
        lambda width, height: [draw_screen(
            screens, scratch, WINDOW_SCREEN, width, height,
            [POINTER_INPUT, str(column), str(row)])])
    if hold:
        hold_screen(qmp, shot, wanted, digests)


def drive_pointer(serial, qmp, scratch, screens, geometry):
    """Over QMP, send the steps of pointer_steps, and after each read the
    lines it waits for, the client's lines checked in order as they come,
    and match the screen where it says; then check that every line came.
    The compositor's pointer lines are checked to have been logged."""
    check_pointer_places(geometry)
    steps = pointer_steps(geometry)
    script = PointerScript(steps)
    serial.script = script
    for step in steps:
        for events in step.events:
            send_events(qmp, events)
        if step.wait:
            serial.wait_for(step.wait, time.monotonic() + KEY_TIMEOUT)
        if step.screen:
            match_pointer(qmp, scratch, screens, geometry, step.screen,
                          step.hold)
    serial.script = None
    script.finish()
    print("          the window was sent %d pointer lines in order, and the "
          "compositor logged each of the pointer's lines"
          % len(script.events), flush=True)


def drive_window(serial, qmp_path, scratch, screens):
    """Over QMP, move the pointer as pointer_steps gives, and then send
    WINDOW_KEYS, and read the window test client's key lines and the
    compositor's attention lines as key_script gives them, until the client
    has logged the last key's, which every key before it has reached the
    window or been taken ahead of."""
    if serial.window is None:
        raise BootFailed("the test client's window line was not read")
    events, attentions = key_script(WINDOW_KEYS)
    script = KeyScript(events, attentions)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(QMP_TIMEOUT)
        connection.connect(qmp_path)
        with connection.makefile("rwb") as stream:
            qmp = Qmp(stream)
            drive_pointer(serial, qmp, scratch, screens,
                          Geometry.of(serial.window))
            serial.script = script
            for step in WINDOW_KEYS:
                print("          input-send-event keys %s" % step,
                      flush=True)
                qmp.execute("input-send-event",
                            events=[key_event(i) for i in step.split()])
            serial.wait_for([events[-1]], time.monotonic() + KEY_TIMEOUT)
    serial.script = None
    script.finish()
    print("          the window was sent %d keys in order, and the "
          "compositor logged the secure attention key %d times"
          % (len(events), len(attentions)), flush=True)


def hold_screen(qmp, shot, wanted, digests):
    """Take QMP's screendump, which it writes to shot, for STEADY_SECONDS
    and fail if one's SHA-256 is not among digests, those of the images
    in wanted."""
    end = time.monotonic() + STEADY_SECONDS
    while time.monotonic() < end:
        qmp.execute("screendump", filename=shot)
        width, height, pixels = read_ppm(shot)
        if hashlib.sha256(pixels).digest() not in digests:
            raise BootFailed("the %dx%d screen changed in %d pixels within "
                             "the %d s it must hold still"
                             % (width, height,
                                min(differing(pixels, image)
                                    for image in wanted), STEADY_SECONDS))
    print("          the screen held still for %d s" % STEADY_SECONDS,
          flush=True)


def check_prompt(qmp, scratch, screens, steady, geometry, first):
    """Match the screen with the one screens draws for login's prompt:
    what login writes, on a first boot, with no account made, and its
    prompt for a name, and at later boots its prompt alone, with the
    pointer where the tablet last was, POINTER_AT, in a window."""
    match_screen(qmp, scratch, screens,
                 [WRITTEN, FIRST_PROMPT if first else LATER_PROMPT], steady,
                 geometry, POINTER_AT)


def attention(serial, qmp):
    """Send the secure attention key, Control and Alt held and Delete
    pressed and released, and wait for the compositor to log it."""
    print("          input-send-event keys %s" % ATTENTION_KEYS_SENT,
          flush=True)
    qmp.execute("input-send-event",
                events=[key_event(item)
                        for item in ATTENTION_KEYS_SENT.split()])
    serial.wait_for([Event(ATTENTION_LINE, at_ms=TIME_MS)],
                    time.monotonic() + KEY_TIMEOUT)


def scroll(serial, qmp, data, screens, scratch, steady, geometry):
    """Write the file of SCROLL_COMMAND, its lines filling the console's
    panel, wait for the shell's prompt after it, then turn the wheel as
    WHEEL_TURNS gives, and match each screen with the one screens draws
    from the lines, the prompt and the turns, as match_screen does with
    steady. In a window, the tablet first moves to WHEEL_AT, inside it,
    which is where the pointer is drawn, and the screen must be as it was
    after the secure attention key."""
    at = POINTER_AT
    if geometry:
        send_events(qmp, move("abs", WHEEL_AT))
        at = WHEEL_AT
    run_commands(serial, qmp, [SCROLL_COMMAND], data)
    inputs = [word for line in SCROLL_LINES + [END_OF_LINES]
              for word in (TYPED_LINE, line)] + [WRITTEN, PROMPT]
    match_screen(qmp, scratch, screens, inputs, steady, geometry, at)
    for detents in WHEEL_TURNS:
        name = "wheel-up" if detents > 0 else "wheel-down"
        for _ in range(abs(detents)):
            print("          input-send-event btn %s" % name, flush=True)
            qmp.execute("input-send-event",
                        events=button(name, True) + button(name, False))
            inputs += [WHEEL_TURNED, "1" if detents > 0 else "-1"]
        match_screen(qmp, scratch, screens, inputs, steady, geometry, at)
    if geometry:
        attention(serial, qmp)
        match_screen(qmp, scratch, screens, inputs, steady, geometry, at)


def stop(qemu):
    if qemu.poll() is None:
        qemu.terminate()
        try:
            qemu.wait(timeout=QUIT_TIMEOUT)
        except subprocess.TimeoutExpired:
            qemu.kill()
            qemu.wait()


def read_u64(disk, offset):
    disk.seek(offset)
    return int.from_bytes(disk.read(8), "little")


def read_u32(disk, offset):
    disk.seek(offset)
    return int.from_bytes(disk.read(4), "little")


def write_u32(disk, offset, value):
    disk.seek(offset)
    disk.write(value.to_bytes(4, "little"))


def flip(disk, offset):
    disk.seek(offset)
    byte = disk.read(1)[0]
    disk.seek(offset)
    disk.write(bytes([byte ^ 0xff]))


def move_to(lba):
    """A change that moves the first entry to start at lba."""
    def change(array, size):
        array[ENTRY_FIRST:ENTRY_FIRST + 8] = lba.to_bytes(8, "little")
    return change


def add_overlap(array, size):
    second = bytearray(array[:size])
    second[:GUID_SIZE] = LINUX_FILESYSTEM
    second[ENTRY_UNIQUE] ^= 0xff
    array[size:2 * size] = second


def rewrite_entries(disk, header, change, first_usable=None):
    """Change the entry array the header at LBA header names, and the
    header's FirstUsableLBA if first_usable is given, and make the
    array's CRC and the header's match."""
    base = header * SECTOR
    entries = read_u64(disk, base + ENTRIES_LBA) * SECTOR
    size = read_u32(disk, base + ENTRY_SIZE)
    length = read_u32(disk, base + ENTRY_COUNT) * size
    disk.seek(entries)
    array = bytearray(disk.read(length))
    change(array, size)
    disk.seek(entries)
    disk.write(array)
    write_u32(disk, base + ENTRIES_CRC, zlib.crc32(array))
    if first_usable is not None:
        disk.seek(base + FIRST_USABLE)
        disk.write(first_usable.to_bytes(8, "little"))
    header_size = read_u32(disk, base + HEADER_SIZE)
    disk.seek(base)
    fields = bytearray(disk.read(header_size))
    fields[HEADER_CRC:HEADER_CRC + 4] = bytes(4)
    write_u32(disk, base + HEADER_CRC, zlib.crc32(fields))


def corrupt_gpt(disk, what):
    """Change both GPT tables as --corrupt-gpt what asks."""
    backup = read_u64(disk, PRIMARY_LBA * SECTOR + ALTERNATE_LBA)
    for header in (PRIMARY_LBA, backup):
        if what == "header":
            flip(disk, header * SECTOR + HEADER_CRC)
        elif what == "entries":
            entries = read_u64(disk, header * SECTOR + ENTRIES_LBA)
            flip(disk, entries * SECTOR + ENTRY_NAME)
        elif what == "overlap":
            rewrite_entries(disk, header, add_overlap)
        elif what == "extent":
            rewrite_entries(disk, header, move_to(0))
        else:
            rewrite_entries(disk, header, move_to(PRIMARY_LBA), PRIMARY_LBA)


def field(data, at, size):
    return int.from_bytes(data[at:at + size], "little")


class Pool:
    """The data volume of the pool on a data disk, read as
    docs/spec/pool-format.md lays it out and include/pool_format.h
    numbers it: the newest valid superblock's tree, with every block
    checked against its pointer's digest."""

    def __init__(self, disk):
        self.disk = disk
        first, last = self.extent()
        self.start = first * SECTOR
        self.blocks = (last - first + 1) * SECTOR // POOL_BLOCK
        root = self.read(self.newest_root())
        self.table_levels = root[ROOT_TABLE_LEVELS]
        self.table = root[ROOT_TABLE:ROOT_TABLE + POINTER_SIZE]

    def extent(self):
        """The first and last LBA of the pool's partition."""
        entries = read_u64(self.disk, PRIMARY_LBA * SECTOR + ENTRIES_LBA)
        count = read_u32(self.disk, PRIMARY_LBA * SECTOR + ENTRY_COUNT)
        size = read_u32(self.disk, PRIMARY_LBA * SECTOR + ENTRY_SIZE)
        for index in range(count):
            self.disk.seek(entries * SECTOR + index * size)
            entry = self.disk.read(size)
            if entry[:GUID_SIZE] == POOL_TYPE:
                return (field(entry, ENTRY_FIRST, 8),
                        field(entry, ENTRY_LAST, 8))
        raise BootFailed("the data disk has no pool partition")

    def block(self, number):
        self.disk.seek(self.start + number * POOL_BLOCK)
        return self.disk.read(POOL_BLOCK)

    def newest_root(self):
        """The root pointer of the valid superblock of the newest
        commit."""
        newest = None
        for ring in (0, self.blocks - POOL_RING_SLOTS):
            for slot in range(POOL_RING_SLOTS):
                block = self.block(ring + slot)
                txg = field(block, SUPER_TXG, 8)
                digest = block[SUPER_DIGEST:SUPER_DIGEST + DIGEST_SIZE]
                if (block[:len(POOL_MAGIC)] == POOL_MAGIC
                        and field(block, SUPER_VERSION, 4) == POOL_VERSION
                        and field(block, SUPER_BLOCK_SIZE, 4) == POOL_BLOCK
                        and field(block, SUPER_BLOCKS, 8) == self.blocks
                        and txg % POOL_RING_SLOTS == slot
                        and hashlib.sha256(block[:SUPER_DIGEST]).digest()
                        == digest
                        and (newest is None or txg > newest[0])):
                    newest = (txg, block[SUPER_ROOT:SUPER_ROOT
                                         + POINTER_SIZE])
        if newest is None:
            raise BootFailed("the data disk holds no valid superblock")
        return newest[1]

    def read(self, pointer):
        """The block pointer names, from a copy its digest matches; a
        hole reads as zeros."""
        first = field(pointer, POINTER_FIRST, 8)
        if first == 0:
            return bytes(POOL_BLOCK)
        for copy in (first, field(pointer, POINTER_SECOND, 8)):
            if copy:
                block = self.block(copy)
                if (hashlib.sha256(block).digest()
                        == pointer[POINTER_DIGEST:POINTER_SIZE]):
                    return block
        raise BootFailed("the data disk's block %d fails its checksum"
                         % first)

    def object_block(self, pointer, levels, index):
        """Block index of the object of levels levels under pointer."""
        if levels == 0:
            return bytes(POOL_BLOCK)
        if index >= POOL_FANOUT ** (levels - 1):
            raise BootFailed("the data disk names a block past its object")
        for level in range(levels, 1, -1):
            span = POOL_FANOUT ** (level - 2)
            at = index // span * POINTER_SIZE
            pointer = self.read(pointer)[at:at + POINTER_SIZE]
            index %= span
        return self.read(pointer)

    def node(self, slot):
        """The kind and bytes of the node at slot: a file's bytes, or a
        directory's entries."""
        block = self.object_block(self.table, self.table_levels,
                                  slot // RECORDS_PER_BLOCK)
        at = slot % RECORDS_PER_BLOCK * RECORD_SIZE
        record = block[at:at + RECORD_SIZE]
        size = field(record, RECORD_SIZE_FIELD, 8)
        pointer = record[RECORD_ROOT:RECORD_ROOT + POINTER_SIZE]
        data = b"".join(self.object_block(pointer, record[RECORD_LEVELS], i)
                        for i in range(-(-size // POOL_BLOCK)))
        return KINDS.get(record[RECORD_KIND], "free"), data[:size]

    def entries(self, slot):
        """The directory at slot's entries: each name's slot and kind."""
        kind, data = self.node(slot)
        if kind != DIRECTORY:
            raise BootFailed("the data disk's node %d is a %s, not a "
                             "directory" % (slot, kind))
        entries = {}
        at = 0
        while at < len(data):
            length = data[at + POOL_ENTRY_LENGTH]
            name = data[at + POOL_ENTRY_NAME:at + POOL_ENTRY_NAME + length]
            entries[name.decode("utf-8")] = (
                field(data, at + POOL_ENTRY_SLOT, 4),
                KINDS.get(data[at + POOL_ENTRY_KIND], "free"))
            at += POOL_ENTRY_NAME + length
        return entries

    def lookup(self, path):
        """The slot of the node at path, a tuple of names below the
        volume's root."""
        slot = ROOT_SLOT
        for depth, name in enumerate(path):
            entries = self.entries(slot)
            if name not in entries:
                raise BootFailed("the data disk has no /%s"
                                 % "/".join(path[:depth + 1]))
            slot = entries[name][0]
        return slot

    def tree(self, slot, prefix=""):
        """Each path below the directory at slot, after prefix, with a
        file's bytes, DIRECTORY or another node's kind."""
        found = {}
        for name, (child, kind) in self.entries(slot).items():
            path = prefix + name
            if kind == DIRECTORY:
                found[path] = DIRECTORY
                found.update(self.tree(child, path + "/"))
            else:
                found[path] = self.node(child)[1] if kind == FILE else kind
        return found


def check_home(path, wanted):
    """Check that the data disk at path holds the account's home alone in
    /users, and that the home holds the tree wanted, no more."""
    with open(path, "rb") as disk:
        pool = Pool(disk)
        users = sorted(pool.entries(pool.lookup(USERS)))
        if users != [ACCOUNT_NAME]:
            raise BootFailed("the data disk's /users holds %s" % users)
        found = pool.tree(pool.lookup(HOME))
    if found != wanted:
        raise BootFailed("the data disk's home holds %s, not %s"
                         % (found, wanted))


def data_disk(args, scratch):
    """A writable copy of the data disk, damaged as --corrupt-gpt asks;
    its drive's arguments, and the lines the boot must then report."""
    if not args.data:
        return [], []
    copy = os.path.join(scratch, DATA_COPY)
    shutil.copyfile(args.data, copy)
    lines = DATA_LINES
    if args.corrupt_gpt:
        with open(copy, "r+b") as disk:
            corrupt_gpt(disk, args.corrupt_gpt)
        lines = [GPT_REFUSED[args.corrupt_gpt], DATA_REFUSED, LOGIN_ABSENT]
    return ["-drive", "if=virtio,format=raw,file=" + copy], lines


def required_tests(args):
    """What a test disk's report must hold, as Ktap.problems takes it: the
    tests that must pass, the lists of tests whose plan must be a given
    count, the tests that must be skipped, and the data results must
    carry. The chain runs only from chain.img and is skipped elsewhere."""
    required = KERNEL_TESTS + INIT_TESTS
    plans, skipped, data = {}, [], {}
    if args.data and not args.corrupt_gpt:
        required.append(FSTEST)
    if args.mode == "chain":
        required += [CHAIN] + list(CHAIN_LINK_DATA)
        plans[CHAIN] = CHAIN_STEPS
        data = CHAIN_LINK_DATA
    else:
        skipped.append(CHAIN)
    return required, plans, skipped, data


def report_problems(report, args):
    """Print the test report, once no more of it will come, and what
    fails it (Ktap.problems)."""
    report.finish()
    for line in report.summary():
        print(line, flush=True)
    problems = report.problems(*required_tests(args))
    for problem in problems:
        print("ktap: " + problem, flush=True)
    return problems


def boot(args, scratch, log, data_drive, data_lines, first):
    """Boot once with data_drive, the data disk's arguments, and check
    the boot's lines, and with --tests its test report; for the
    specimen and the window, check its screen; in system and persist
    modes, send the
    pointer's events, and with a data disk whose GPT is whole create the
    account if first or log in with it, and run whoami and the file
    commands."""
    vars_copy = os.path.join(scratch, "vars.fd")
    shutil.copyfile(args.vars, vars_copy)
    qmp_path = os.path.join(scratch, "qmp.sock")
    accounts = (args.mode not in ("chain", "specimen", "window")
                and data_drive and not args.corrupt_gpt)
    screens = None
    if (accounts and args.mode in ("system",) + GIVEN_UP_MODES
            or args.mode == "window"):
        screens = build_screens(scratch)
    ready = []
    if accounts:
        ready = FIRST_BOOT_READY if first else LATER_BOOT_READY
    command = (
        ["qemu-system-x86_64", "-machine", "q35", "-m", MEMORY,
         "-drive", "if=pflash,format=raw,readonly=on,file=" + args.code,
         "-drive", "if=pflash,format=raw,file=" + vars_copy,
         "-drive", "if=virtio,format=raw,readonly=on,file=" + args.disk]
        + data_drive
        + DEVICES
        + (size_arguments(args.size) if args.size else [])
        + ["-display", "none", "-serial", "stdio",
           "-qmp", "unix:%s,server=on,wait=off" % qmp_path]
        + args.qemu_args)
    print(" ".join(command), flush=True)
    qemu = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE)
    serial = None
    report = Ktap() if args.tests else None
    try:
        watches = []
        if args.mode == "fallback":
            watches = [(COMPOSITOR_DISPLAY,
                        lambda: check_limb(qmp_path, scratch, screens))]
        if args.mode == "window":
            watches = [(WINDOW_SHOWN,
                        lambda: check_window(qmp_path, scratch, screens))]
        serial = Serial(qemu.stdout, log, report, args.size, args.mode,
                        watches)
        lines = {"chain": CHAIN_LINES,
                 "specimen": SPECIMEN_LINES,
                 "window": WINDOW_LINES,
                 "fallback": FALLBACK_STARTUP_LINES + FALLBACK_LINES,
                 "noanswer": FALLBACK_STARTUP_LINES + NO_ANSWER_LINES}.get(
                     args.mode, SYSTEM_LINES + COMPOSITOR_DRAWN[args.tests])
        if not args.tests:
            lines = [RANDOM_PASSED] + lines
        try:
            serial.wait_for(lines + data_lines + ready + args.expect,
                            serial.start + args.timeout,
                            report_done=bool(report))
        except BootFailed:
            if report:
                report_problems(report, args)
            raise
        if report and report_problems(report, args):
            raise BootFailed("the test report fails")
        if args.mode == "specimen":
            check_specimen(qmp_path, scratch, args.fonts, args.screen)
        if args.mode == "window":
            drive_window(serial, qmp_path, scratch, screens)
        if args.mode in ("system", "persist") + GIVEN_UP_MODES:
            drive(serial, qmp_path, os.path.join(scratch, DATA_COPY),
                  accounts, first, screens, scratch, args.tests,
                  serial.window if args.mode in WINDOW_PATH_MODES else None)
        if args.mode == "logout":
            drive_logout(serial, qmp_path)
    finally:
        stop(qemu)
        if serial:
            serial.reader.join()
        qemu.stdout.close()
    if serial.leaked:
        raise BootFailed("the serial log holds "
                         + "; ".join(sorted(serial.leaked)))


def expected_line(text):
    """--expect's LINE as an Event."""
    if not parse_line(text):
        raise argparse.ArgumentTypeError(
            "not a line of the form component: event key=value ...: %r"
            % text)
    return Event(text)


def main():
    parser = argparse.ArgumentParser(
        description="Boot a nova-os disk under QEMU and check its serial log.")
    parser.add_argument("--code", required=True,
                        help="the firmware's code, read only")
    parser.add_argument("--vars", required=True,
                        help="the firmware's variable store, copied")
    parser.add_argument("--log", required=True,
                        help="where to write the serial log")
    parser.add_argument("--timeout", type=float,
                        help="seconds the boot may take")
    parser.add_argument("--expect", action="append", default=[],
                        type=expected_line, metavar="LINE",
                        help="a line the boot must report, in the log's "
                        "form, with the keys it gives")
    parser.add_argument("--tests", action="store_true",
                        help="the disk is a test disk, whose boot must "
                        "report its tests, all passing")
    parser.add_argument("--data",
                        help="the data disk, attached as a writable copy")
    parser.add_argument("--corrupt-gpt", choices=sorted(GPT_REFUSED),
                        help="change a byte of the data disk's GPT")
    parser.add_argument("--size", type=screen_size, metavar="WIDTHxHEIGHT",
                        help="the display's size in pixels, which the "
                        "console's display line must name")
    parser.add_argument("--fonts",
                        help="the nova-os fonts the specimen's prototype "
                        "draws with")
    parser.add_argument("--screen",
                        help="where to keep the specimen's screendump")
    parser.add_argument("mode", choices=sorted(DEFAULT_TIMEOUTS))
    parser.add_argument("disk")
    parser.add_argument("qemu_args", nargs="*", metavar="QEMU_ARG")
    args = parser.parse_args()
    if args.corrupt_gpt and not args.data:
        parser.error("--corrupt-gpt needs --data")
    if (args.mode in ("persist",) + GIVEN_UP_MODES
            and (not args.data or args.corrupt_gpt)):
        parser.error("%s needs --data, whole" % args.mode)
    if (args.mode in ("chain", "specimen", "window") + GIVEN_UP_MODES
            and not args.tests):
        parser.error("%s takes only a test disk, with --tests" % args.mode)
    if args.mode == "logout" and (args.tests or not args.data
                                  or args.corrupt_gpt):
        parser.error("logout takes a data disk, whole, and no --tests")
    if (args.mode == "specimen") != bool(args.fonts):
        parser.error("specimen needs --fonts, and only specimen takes it")
    if args.screen and args.mode != "specimen":
        parser.error("only specimen takes --screen")
    if args.size and args.mode not in (("system", "window", "persist")
                                       + GIVEN_UP_MODES):
        parser.error("only system, fallback, noanswer, window and persist "
                     "take --size")
    if args.timeout is None:
        args.timeout = DEFAULT_TIMEOUTS[args.mode]
    with tempfile.TemporaryDirectory() as scratch, \
            open(args.log, "wb") as log:
        try:
            data_drive, data_lines = data_disk(args, scratch)
            boot(args, scratch, log, data_drive, data_lines, first=True)
            if args.mode == "persist":
                print("booting the same data disk again", flush=True)
                boot(args, scratch, log, data_drive, data_lines, first=False)
        except (BootFailed, OSError) as failure:
            print("FAILED: %s" % failure, flush=True)
            return 1
    print("passed", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
