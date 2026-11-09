"""Check which lines ci/boot.py's fallback boot requires and accepts.

Usage: python3 -I ci/check-boot.py

Replays serial lines through boot.py's Serial, as a boot reads them, and
checks what boot.py makes of them: the lines docs/spec/display.md D19's
rule 9 has the fallback boot report, each as often as it gives, and the
same boot with the input drivers' ends read before the compositor's; the
failure lines the fallback boot accepts, each as often as it gives and
each alone, and no failure line in any other mode; the console's display
lines of the other boots, which a display after the compositor is not;
the lines of the window boot, each of which it requires, and a failure
line of the test client, which fails it; the compositor's refusals of an
endpoint's message and the console's lines for an endpoint, which fail it;
where boot.py puts the window boot's pointer places on the screens the boots
run, against places worked out by hand here from display.md D6, D10 and
D11, and the steps taken through them: the lines the client must log for
each, which PointerScript reads in order, so that a line lost, doubled,
swapped, changed in a field or without its time, a time that falls, a line
after the last, and a pointer line of the compositor's that was not logged
each fail the boot, and so does a pointer line of the client while the
keys are typed; the three screens the steps must match, the last held; the
window line's keys, which Serial keeps, and the places' numbers, which the
compositor's pointer lines must not hold;
how boot.py's KeyScript reads a log of the keys the window boot sends: a
client key line that is lost, doubled, out of order, with another code,
value, modifier set or text, or without its time, an attention line that
is lost, doubled or after the client's line for the key sent after it,
the attention key's own press logged by the client, and a time that falls
or that lies outside its neighbours' (an attention time before the
client's line for the key before it or after that for the key after it),
each fail the boot, and an attention line logged earlier than it must be
does not. The logs are built from key_script's own events, so these
checks show how KeyScript reads a log against that model, not that the
model is what the compositor and the client do: the model is written from
display.md D11 and D12, and the boot is what tests the compositor against
it; the screens a boot waits for, which poll_screen takes from QMP's
screendump until one is the wanted image or SCREEN_TIMEOUT passes; and
the usage errors of a mode that needs a whole data disk and those of a
mode that takes none. Prints each check that fails, and a count; the
exit status is nonzero if any did.
"""

import contextlib
import importlib.util
import io
import os
import pathlib
import sys
import tempfile
import time
import unittest.mock

BOOT_PATH = pathlib.Path(__file__).with_name("boot.py")
REPLAY_SECONDS = 10
FUNCTIONS = ["0000:00:03.0", "0000:00:04.0", "0000:00:05.0"]
OTHER_FUNCTION = "0000:00:06.0"
FALLBACK = "fallback"
SENTINEL = "nova: startup done"
USAGE_ERROR = 2
WINDOW = "window"

REFUSED = 'nova-compositor: background refused reason="another size"'
DISPLAY = ("nova-compositor: display screen=1280x800 wallpaper=%s "
           "prepare_ms=1.0 first_frame_ms=1.0")
ENDED = "nova-init: child ended name=compositor exit=%d"
COMPOSITOR_RESTARTED = "nova-init: child restarted name=compositor"
COMPOSITOR_GIVEN_UP = ('nova-init: child given up name=compositor '
                       'reason="down again within the restart period"')
CLIENT_RESTARTED = "its client restarted"
CLIENT_CHANGED = "its client changed"
DRIVER_ENDED = "nova-init: child ended name=virtio-input pci=%s exit=100"
DRIVER_WAITING = ("nova-init: child waiting name=virtio-input pci=%s "
                  "reason=\"its client's end is not read\"")
CONSOLE = ("nova-console: display screen=1280x800 cell=8x16 grid=80x30 "
           "backdrop=%s prepare_ms=1.0 draw_ms=1.0%s")
CONNECTION = ("nova-compositor: connection issued number=1 label=%s mark=no "
              "layers=%s rights=none")
WINDOW_CONNECTION = CONNECTION % ("wintest", "system")
WINDOW_MADE = "nova-wintest: window screen=1280x800 size=640x400"
COMPOSITOR_INPUT = ["nova-compositor: input kind=keyboard",
                    "nova-compositor: input kind=pointer",
                    "nova-compositor: input kind=pointer x=0..32767 "
                    "y=0..32767"]
INPUT_REFUSALS = ["nova-compositor: input message refused status=0",
                  "nova-console: input kind=keyboard"]
KEY_TIME_STEP = 3.0
ATTENTION = "nova-compositor: attention"
SHOWN = "nova-wintest: shown frame=%d"
RELEASED = "nova-wintest: released buffers=%d"
SHOWN_RELEASED = RELEASED % 1
STORE_SERVING = "nova-store: serving"
BACKGROUND = "nova: background prepare_ms=1.0 spinner_frames=3"
WINDOW_FAILED = "nova-wintest: failed step=\"window request\" status=15"
WINDOW_CLOSED = "nova-compositor: window closed label=wintest reason=stalled"
CONNECTION_CLOSED = ("nova-compositor: connection closed number=1 "
                     "label=wintest reason=malformed")
PIXEL_BYTES = 3
SCREEN_WIDTH = 4
SCREEN_HEIGHT = 2


def load_boot():
    spec = importlib.util.spec_from_file_location("boot", BOOT_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def driver_restarted(function, reason, name="virtio-input"):
    line = "nova-init: child restarted name=%s" % name
    if function:
        line += " pci=" + function
    return line + (' reason="%s"' % reason if reason else "")


def fallback_log(boot):
    """The lines of the fallback boot that these checks are about, in the
    order it reports them: each of the compositor's two starts, with the
    drivers started again between them and after them, and the console's
    display."""
    exit_code = boot.COMPOSITOR_EXIT_TEST
    return ([REFUSED, DISPLAY % "computed", ENDED % exit_code,
             COMPOSITOR_RESTARTED]
            + [driver_restarted(f, CLIENT_RESTARTED) for f in FUNCTIONS]
            + [DISPLAY % "computed", ENDED % exit_code, COMPOSITOR_GIVEN_UP]
            + [driver_restarted(f, CLIENT_CHANGED) for f in FUNCTIONS]
            + [CONSOLE % ("computed", " after=compositor")])


def drivers_first_log(boot):
    """The lines of the fallback boot when init reads each input driver's
    end before the compositor's, which the kernel's order of closing a
    process's handles and asserting its TERMINATED allows: each driver
    ends and waits for the compositor's end, and is started again for it
    once that is read."""
    exit_code = boot.COMPOSITOR_EXIT_TEST

    def ends():
        return [line for f in FUNCTIONS
                for line in (DRIVER_ENDED % f, DRIVER_WAITING % f)]

    return ([REFUSED, DISPLAY % "computed"] + ends()
            + [ENDED % exit_code, COMPOSITOR_RESTARTED]
            + [driver_restarted(f, CLIENT_RESTARTED) for f in FUNCTIONS]
            + [DISPLAY % "computed"] + ends()
            + [ENDED % exit_code, COMPOSITOR_GIVEN_UP]
            + [driver_restarted(f, CLIENT_CHANGED) for f in FUNCTIONS]
            + [CONSOLE % ("computed", " after=compositor")])


def window_log():
    """The lines of the window boot that these checks are about, in the
    order it reports them, but for startup done, which the replay ends
    with."""
    return [STORE_SERVING, BACKGROUND, REFUSED, DISPLAY % "computed",
            WINDOW_CONNECTION, WINDOW_MADE, SHOWN % 1, SHOWN_RELEASED,
            SHOWN % 2] + COMPOSITOR_INPUT


def text_value(text):
    """text as a log value: bare, or in quotes where it is empty or holds
    a space or a quote."""
    return text if text and " " not in text and '"' not in text \
        else '"%s"' % text


def key_line(event, time):
    """The line the window test client logs for the key event, an Event
    of key_script, at time ms."""
    keys = event.keys
    return ("nova-wintest: key code=%s value=%s modifiers=%s text=%s "
            "time_ms=%.1f" % (keys["code"], keys["value"], keys["modifiers"],
                              text_value(keys["text"]), time))


def attention_line(time):
    return "%s at_ms=%.1f" % (ATTENTION, time)


def key_log(boot):
    """The lines of the window boot's keys, in the order the boot reads
    them: each key line of key_script's, KEY_TIME_STEP ms after the one
    before, and each attention line just before the line of the first
    key sent after it, at the middle of the time between the keys around
    it. Returns the lines, the keys, the attentions and the keys' times."""
    events, attentions = boot.key_script(boot.WINDOW_KEYS)
    times = [KEY_TIME_STEP * (n + 1) for n in range(len(events))]
    lines = []
    for index, event in enumerate(events):
        lines += [attention_line(times[index] - KEY_TIME_STEP / 2)
                  for count in attentions if count == index]
        lines.append(key_line(event, times[index]))
    return lines, events, attentions, times


def replay_keys(boot, lines):
    """Whether Serial reads lines, as the window boot's, with the
    KeyScript of boot.WINDOW_KEYS set, finds the last key's line, and
    the script finishes, without failing the boot."""
    events, attentions = boot.key_script(boot.WINDOW_KEYS)
    script = boot.KeyScript(events, attentions)
    stream = [(line + "\r\n").encode("ascii") for line in lines + [SENTINEL]]
    serial = boot.Serial(stream, io.BytesIO(), None, None, WINDOW)
    serial.script = script
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            serial.wait_for([events[-1], boot.Event(SENTINEL)],
                            time.monotonic() + REPLAY_SECONDS)
        script.finish()
    except boot.BootFailed:
        return False
    finally:
        serial.reader.join()
    return True

# The screens the boots run, as the client's window line gives them, and
# the window it asks for, half the screen across and down.
WINDOW_SCREENS = [(800, 600), (1280, 800), (1366, 768), (2560, 1600),
                  (2880, 1800)]
CLIENT_TIME_STEP = 0.5


def window_keys(width, height):
    """The keys of the client's window line on a screen."""
    return {"screen": "%dx%d" % (width, height),
            "size": "%dx%d" % (width // 2, height // 2)}


def event_line(event, time):
    """The line the test client logs for an Event of pointer_steps, at time
    ms: its keys as the Event has them, and a time where it asks for one."""
    keys = "".join(" %s=%s" % (key, value) for key, value in event.keys.items()
                   if key != "time_ms")
    clock = " time_ms=%.1f" % time if "time_ms" in event.keys else ""
    return "%s: %s%s%s" % (event.component, event.event, keys, clock)


def pointer_log(boot, steps):
    """The lines of the window boot's pointer, in the order the boot reads
    them: the compositor's line for each of the pointer's events, which
    comes first, and then the client's line for each of the steps' lines,
    each CLIENT_TIME_STEP ms after the one before. Returns the lines, those
    of the client and their times."""
    compositor = [event.text for event in boot.pointer_lines(boot.COMPOSITOR)]
    events = [line for step in steps for line in step.lines]
    times = [CLIENT_TIME_STEP * (n + 1) for n in range(len(events))]
    client = [event_line(event, time) for event, time in zip(events, times)]
    return compositor + client, client, times


def replay_pointer(boot, steps, lines):
    """Whether Serial reads lines, as the window boot's, with the
    PointerScript of steps set, finds the client's line for the last of the
    steps' lines, and the script finishes, without failing the boot."""
    script = boot.PointerScript(steps)
    last = [line for step in steps for line in step.lines][-1]
    stream = [(line + "\r\n").encode("ascii") for line in lines + [SENTINEL]]
    serial = boot.Serial(stream, io.BytesIO(), None, None, WINDOW)
    serial.script = script
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            serial.wait_for([last, boot.Event(SENTINEL)],
                            time.monotonic() + REPLAY_SECONDS)
        script.finish()
    except boot.BootFailed:
        return False
    finally:
        serial.reader.join()
    return True


def serial_after(boot, mode, lines):
    """The Serial that has read lines, then the sentinel, in mode."""
    stream = [(line + "\r\n").encode("ascii") for line in lines + [SENTINEL]]
    serial = boot.Serial(stream, io.BytesIO(), None, None, mode)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            serial.wait_for([boot.Event(SENTINEL)],
                            time.monotonic() + REPLAY_SECONDS)
    finally:
        serial.reader.join()
    return serial


def ppm(fill):
    """A binary PPM of SCREEN_WIDTH x SCREEN_HEIGHT pixels, each fill, a
    byte, in every channel."""
    return (b"P6\n%d %d\n255\n" % (SCREEN_WIDTH, SCREEN_HEIGHT)
            + bytes([fill]) * (PIXEL_BYTES * SCREEN_WIDTH * SCREEN_HEIGHT))


class Screendumps:
    """QMP's screendump as a test double: each dump writes the next of
    shots, a PPM, to the file asked for, and the last is written again
    from then on."""

    def __init__(self, shots):
        self.shots = list(shots)
        self.dumps = 0

    def execute(self, command, **arguments):
        assert command == "screendump"
        shot = self.shots[min(self.dumps, len(self.shots) - 1)]
        self.dumps += 1
        with open(arguments["filename"], "wb") as out:
            out.write(shot)


def polled(boot, shots, wanted, timeout=REPLAY_SECONDS):
    """The images poll_screen returns, as bytes, and how many dumps it
    took, for the dumps in shots and the wanted fills; or None if it
    gives up, after timeout seconds, with BootFailed."""
    qmp = Screendumps(shots)
    with tempfile.TemporaryDirectory() as scratch, \
            unittest.mock.patch.object(boot, "SCREEN_TIMEOUT", timeout), \
            contextlib.redirect_stdout(io.StringIO()):
        try:
            images, _ = boot.poll_screen(
                qmp, os.path.join(scratch, "screen.ppm"),
                lambda width, height: [
                    bytes([fill]) * (PIXEL_BYTES * width * height)
                    for fill in wanted])
        except boot.BootFailed:
            return None
    return images, qmp.dumps


def failure_lines(log):
    """The lines of log that report a failure, which the fallback boot
    accepts."""
    return [line for line in log
            if "child restarted" in line or "child given up" in line]


def replay(boot, mode, lines, wanted=()):
    """Whether Serial reads lines, then the sentinel, and finds each
    Event in wanted reported as often as it must be, without failing the
    boot, which accepts the failures of mode and refuses the lines it
    refuses."""
    stream = [(line + "\r\n").encode("ascii") for line in lines + [SENTINEL]]
    serial = boot.Serial(stream, io.BytesIO(), None, None, mode)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            serial.wait_for(list(wanted) + [boot.Event(SENTINEL)],
                            time.monotonic() + REPLAY_SECONDS)
    except boot.BootFailed:
        return False
    finally:
        serial.reader.join()
    return True


def usage_error(boot, arguments, tests=True):
    """The status and the message boot.main exits with for arguments,
    with --tests unless tests is false, or None for the status where it
    goes on to a boot, which is not made here."""
    command = (["boot.py", "--code", "code.fd", "--vars", "vars.fd",
                "--log", os.devnull] + (["--tests"] if tests else [])
               + arguments)
    message = io.StringIO()
    not_made = boot.BootFailed("not a usage error")
    with unittest.mock.patch.object(sys, "argv", command), \
            unittest.mock.patch.object(boot, "data_disk",
                                       side_effect=not_made), \
            unittest.mock.patch.object(boot, "boot", side_effect=not_made), \
            contextlib.redirect_stdout(io.StringIO()), \
            contextlib.redirect_stderr(message):
        try:
            boot.main()
        except SystemExit as exit_status:
            return exit_status.code, message.getvalue()
    return None, message.getvalue()


def main():
    boot = load_boot()
    parse = boot.parse_line
    failures = []
    checks = 0

    def expect(name, outcome, wanted=True):
        nonlocal checks
        checks += 1
        if outcome != wanted:
            failures.append(name)

    def replayed(name, mode, lines, wanted=()):
        expect(name, replay(boot, mode, lines, wanted), True)

    def refused(name, mode, lines, wanted=()):
        expect(name, replay(boot, mode, lines, wanted), False)

    log = fallback_log(boot)
    required = boot.FALLBACK_LINES

    # The failure lines are the fallback boot's alone.
    for mode in sorted(boot.DEFAULT_TIMEOUTS):
        expect("%s mode accepts the fallback boot's failure lines" % mode,
               replay(boot, mode, failure_lines(log)), mode == FALLBACK)
        expect("%s mode accepts failures as the fallback boot does" % mode,
               boot.accepted_failures(mode) == boot.ACCEPTED_FAILURES,
               mode == FALLBACK)

    # The whole fallback boot is reported, each line as often as it
    # gives, and no line of it can be missing.
    replayed("the fallback boot's lines are reported", FALLBACK, log,
             required)
    for index, line in enumerate(log):
        name = "the fallback boot without line %d (%s)" % (index, line[:48])
        without = log[:index] + log[index + 1:]
        if CLIENT_CHANGED in line:
            replayed(name + ", which it does not require", FALLBACK,
                     without, required)
        else:
            refused(name, FALLBACK, without, required)
    replayed("the fallback boot with no driver started again for the "
             "console", FALLBACK,
             [line for line in log if CLIENT_CHANGED not in line], required)
    ended = [i for i, line in enumerate(log) if "exit=" in line]
    displays = [i for i, line in enumerate(log)
                if line.startswith("nova-compositor: display")]
    for position, index in enumerate(ended):
        for code in (boot.COMPOSITOR_EXIT_TEST - 1, 3):
            changed = list(log)
            changed[index] = ENDED % code
            refused("the compositor's end %d with exit=%d"
                    % (position + 1, code), FALLBACK, changed, required)
    for position, index in enumerate(displays):
        for name, wallpaper in (("section", "section"), ("absent", None)):
            changed = list(log)
            changed[index] = (DISPLAY % wallpaper if wallpaper
                              else DISPLAY.replace(" wallpaper=%s", ""))
            refused("the compositor's display line %d with its wallpaper "
                    "%s" % (position + 1, name), FALLBACK, changed,
                    required)
    changed = list(log)
    changed[-1] = CONSOLE % ("computed", "")
    refused("a console display without after=compositor", FALLBACK,
            changed, required)
    changed[-1] = CONSOLE % ("section", " after=compositor")
    refused("a console display after the compositor with backdrop=section",
            FALLBACK, changed, required)

    # The drivers' ends read before the compositor's give the same boot,
    # and a driver that waits is no failure in any mode.
    replayed("the fallback boot's lines are reported with the drivers' ends "
             "read first", FALLBACK, drivers_first_log(boot), required)
    for mode in sorted(boot.DEFAULT_TIMEOUTS):
        replayed("a driver that waits is no failure in %s mode" % mode, mode,
                 [DRIVER_ENDED % FUNCTIONS[0], DRIVER_WAITING % FUNCTIONS[0]])

    # Each accepted failure line is accepted alone, and no line that is
    # not one is, alone.
    accepted = [COMPOSITOR_RESTARTED, COMPOSITOR_GIVEN_UP,
                driver_restarted(FUNCTIONS[0], CLIENT_RESTARTED),
                driver_restarted(FUNCTIONS[0], CLIENT_CHANGED)]
    for line in accepted:
        replayed("%s alone is accepted" % line[:60], FALLBACK, [line])
        refused("%s twice is not" % line[:60], FALLBACK, [line, line])
    others = [
        "nova-init: child restarted name=console",
        "nova-init: child restarted name=virtio-blk",
        "nova-init: child restarted name=compositor-x",
        "nova-init: child given up name=console",
        "nova-init: child given up name=virtio-blk",
        driver_restarted(FUNCTIONS[0], None),
        driver_restarted(FUNCTIONS[0], "its function"),
        driver_restarted(FUNCTIONS[0], CLIENT_CHANGED, "virtio-blk"),
        driver_restarted(FUNCTIONS[0], CLIENT_RESTARTED, "console"),
        driver_restarted(None, CLIENT_CHANGED),
        driver_restarted(None, CLIENT_RESTARTED),
        ("nova-init: child given up name=virtio-input pci=%s reason=\"%s\""
         % (FUNCTIONS[0], CLIENT_CHANGED)),
        "nova: halted",
        "nova-compositor: background send failed",
    ]
    for line in others:
        refused("%s alone is not accepted" % line[:70], FALLBACK, [line])

    # An input driver is started again once for each reason, for each of
    # the machine's input functions and no other.
    for reason in (CLIENT_RESTARTED, CLIENT_CHANGED):
        every = [driver_restarted(f, reason) for f in FUNCTIONS]
        replayed("a restart for %s of each function is accepted" % reason,
                 FALLBACK, every)
        refused("a restart for %s of a fourth function is not" % reason,
                FALLBACK, every + [driver_restarted(OTHER_FUNCTION,
                                                    reason)])
        refused("a restart for %s of a function twice is not" % reason,
                FALLBACK, every + [every[1]])
    refused("a restart of a fourth function for the other reason is not",
            FALLBACK, [driver_restarted(f, CLIENT_RESTARTED)
                       for f in FUNCTIONS]
            + [driver_restarted(OTHER_FUNCTION, CLIENT_CHANGED)])
    refused("a restart of no function is not accepted after the three",
            FALLBACK, failure_lines(log) + [driver_restarted(None,
                                                             CLIENT_CHANGED)])

    # The console's display lines of the other boots are not a display
    # after the compositor, and a display after it is none of theirs.
    drawn = boot.CONSOLE_DRAWN
    plain = {backdrop: parse(CONSOLE % (backdrop, ""))
             for backdrop in ("section", "computed")}
    after = {backdrop: parse(CONSOLE % (backdrop, " after=compositor"))
             for backdrop in ("section", "computed")}
    other = {"section": drawn[False][0], "computed": drawn[True][1]}
    for backdrop in ("section", "computed"):
        expect("a %s display is the system boot's" % backdrop,
               other[backdrop].matches(plain[backdrop]), True)
        expect("a %s display after the compositor is not the system "
               "boot's" % backdrop,
               other[backdrop].matches(after[backdrop]), False)
    expect("a computed display after the compositor is the fallback "
           "boot's", boot.CONSOLE_AFTER.matches(after["computed"]), True)
    expect("a computed display is not the fallback boot's",
           boot.CONSOLE_AFTER.matches(plain["computed"]), False)
    expect("a section display after the compositor is not the fallback "
           "boot's", boot.CONSOLE_AFTER.matches(after["section"]), False)

    # The window boot reports each of its lines, and none can be missing.
    # A line that is not the one asked for, in the keys it must have, is
    # not it.
    window = window_log()
    window_required = boot.WINDOW_LINES
    replayed("the window boot's lines are reported", WINDOW, window,
             window_required)
    for index, line in enumerate(window):
        refused("the window boot without line %d (%s)" % (index, line[:48]),
                WINDOW, window[:index] + window[index + 1:], window_required)
    for name, old, new in (
            ("a connection of another label", WINDOW_CONNECTION,
             CONNECTION % ("console", "system")),
            ("a connection with other layers", WINDOW_CONNECTION,
             CONNECTION % ("wintest", "system,app")),
            ("a release of another buffer", SHOWN_RELEASED, RELEASED % 2),
            ("a first frame in place of the second", SHOWN % 2, SHOWN % 1),
            ("a window with no size", WINDOW_MADE,
             WINDOW_MADE.replace(" size=640x400", ""))):
        refused("the window boot with " + name, WINDOW,
                [new if line == old else line for line in window],
                window_required)
    refused("the window boot with a failure of its client", WINDOW,
            window + [WINDOW_FAILED], window_required)

    # The client holds its window and its connection for the whole boot,
    # and a close by its holder logs nothing, so a line that says either
    # closed, wherever it comes, is the compositor's, and fails the boot.
    # It fails no other mode's, which have no such client.
    after = window.index(WINDOW_CONNECTION) + 1
    for name, line in (("its window closed", WINDOW_CLOSED),
                       ("its connection closed", CONNECTION_CLOSED)):
        refused("the window boot with %s at its end" % name, WINDOW,
                window + [line], window_required)
        refused("the window boot with %s before its window is made" % name,
                WINDOW, window[:after] + [line] + window[after:],
                window_required)
        for mode in sorted(boot.DEFAULT_TIMEOUTS):
            expect("%s mode refuses a line that says %s" % (mode, name),
                   replay(boot, mode, [line]), mode != WINDOW)
    expect("the window boot refuses the close lines and no other boot does",
           [mode for mode in sorted(boot.DEFAULT_TIMEOUTS)
            if boot.refused_lines(mode)], [WINDOW])

    # The compositor reads each endpoint and the console none, so a line
    # of the console for an endpoint, or a message the compositor refuses,
    # fails the window boot, wherever it comes.
    for line in INPUT_REFUSALS:
        refused("the window boot with %s at its end" % line[:60], WINDOW,
                window + [line], window_required)
        refused("the window boot with %s before its window is made"
                % line[:60], WINDOW,
                window[:after] + [line] + window[after:], window_required)

    # A log built from key_script's events is read as the boot reads it:
    # the check of KeyScript against its own model's output, which is
    # what it shows and no more. The counts are written out by hand from
    # WINDOW_KEYS, the keys that reach the window and the presses of the
    # secure attention key, which reach none.
    lines, events, attentions, times = key_log(boot)
    count = len(events)
    expect("the key script has the events and the attention presses "
           "WINDOW_KEYS gives it, counted by hand",
           (count, len(attentions)), (61, 4))
    expect("a log of the key script's events and attention lines is read "
           "in order", replay_keys(boot, lines), True)

    def keyed(name, changed, accepted=False):
        expect("the window boot's keys " + name, replay_keys(boot, changed),
               accepted)

    positions = [i for i, line in enumerate(lines)
                 if line.startswith("nova-wintest: key")]
    for position in positions:
        line = lines[position]
        name = "with the key line %s" % line[len("nova-wintest: key "):56]
        keyed(name + " lost", lines[:position] + lines[position + 1:])
        keyed(name + " twice",
              lines[:position + 1] + lines[position:])
        for field, other in (("code", "1"), ("value", "repeated"),
                             ("modifiers", "super"), ("text", "?")):
            old = [word for word in line.split() if word.startswith(field
                                                                    + "=")]
            if field == "text":
                old = [line[line.index(" text=") + 1:line.index(" time_ms")]]
            changed = line.replace(old[0], "%s=%s" % (field, other))
            keyed(name + " with another " + field,
                  lines[:position] + [changed] + lines[position + 1:])
        keyed(name + " with no time",
              lines[:position] + [line[:line.index(" time_ms")]]
              + lines[position + 1:])
    for first, second in zip(positions, positions[1:]):
        if first + 1 == second and (lines[first].split(" time_ms")[0]
                                    != lines[second].split(" time_ms")[0]):
            swapped = list(lines)
            swapped[first], swapped[second] = lines[second], lines[first]
            keyed("with the lines at %d and %d swapped" % (first, second),
                  swapped)
    keyed("with a key line after the last", lines + [lines[-1]])

    # The secure attention key's press, repeats and release reach no
    # window: a key line for any of them fails, and each press is logged
    # by the compositor once, before the line of the key sent after it, or
    # earlier, as the compositor, ahead of the client, may log it.
    attention_positions = [i for i, line in enumerate(lines)
                           if line.startswith(ATTENTION)]
    for number, position in enumerate(attention_positions):
        name = "with the attention line %d" % (number + 1)
        keyed(name + " lost", lines[:position] + lines[position + 1:])
        keyed(name + " twice", lines[:position + 1] + lines[position:])
        keyed(name + " after the line of the key sent after it",
              lines[:position] + [lines[position + 1], lines[position]]
              + lines[position + 2:])
        ahead = attention_positions[number - 1] + 1 if number else 0
        keyed(name + " logged earlier, ahead of the client",
              lines[:ahead] + [lines[position]] + lines[ahead:position]
              + lines[position + 1:], True)
        leaked = key_line(boot.Event(
            "nova-wintest: key", code="111", value="pressed",
            modifiers="control,alt", text=""), times[attentions[number]])
        keyed(name + " and Delete's press sent to the window",
              lines[:position] + [leaked] + lines[position:])
    # The first attention press comes with a Delete held that the window was
    # sent the press of, which the attention key's press ends: that Delete's
    # release is not sent either.
    after_first = attention_positions[0] + 1
    stale = key_line(boot.Event(
        "nova-wintest: key", code="111", value="released",
        modifiers="control,alt", text=""), times[attentions[0]])
    keyed("with the release of a Delete sent before the attention key's "
          "press of it sent to the window",
          lines[:after_first] + [stale] + lines[after_first:])
    keyed("with an attention line after the last key",
          lines + [attention_line(times[-1])])
    keyed("with every attention line before the keys",
          [lines[i] for i in attention_positions]
          + [line for i, line in enumerate(lines)
             if i not in attention_positions], True)

    # The times: those of the client's lines do not fall, and each
    # attention line's lies between those of the keys around it.
    first = positions[0]
    keyed("with a time that falls", lines[:first + 1] + [
        lines[first + 1].replace("time_ms=%.1f" % times[1],
                                 "time_ms=%.1f" % (times[0] - 1))]
          + lines[first + 2:])
    for number, position in enumerate(attention_positions):
        near = attentions[number]
        for name, moved in (("before", times[near - 1] - 1),
                            ("after", times[near] + 1)):
            keyed("with attention line %d at a time %s the keys around it"
                  % (number + 1, name),
                  lines[:position] + [attention_line(moved)]
                  + lines[position + 1:])

    # The places the window boot sends the tablet to. The numbers for
    # 1280 x 800 are worked out here by hand from D6, D10 and D11: a
    # window of 640 x 400 has its left at (1280 - 640) / 2 = 320 and its
    # top at 800 * 40 / 100 - 400 / 2 = 120; the tablet's value across is
    # in 32768ths of the screen's width, so 15991 is 15991 * 1280 * 256 /
    # 32768 = 159910 256ths of a pixel, the pixel 624, and 159910 - 320 *
    # 256 = 77990 from the window's left; down it is 13003 * 800 * 256 /
    # 32768 = 81268, the pixel 317, and 81268 - 120 * 256 = 50548 from the
    # window's top.
    geometry = boot.Geometry.of(window_keys(1280, 800))
    expect("the window of 640 x 400 on 1280 x 800 is at 320, 120",
           (geometry.left, geometry.top), (320, 120))
    expect("the entry's place is the pixel 624, 317, and 77990 and 50548 "
           "from the window's top left",
           (geometry.pixel(boot.ENTRY), geometry.relative(boot.ENTRY)),
           ((624, 317), (77990, 50548)))
    expect("the entry's place is over the window and the places outside and "
           "below it are not",
           (geometry.over_window(boot.ENTRY),
            geometry.over_window(boot.OUTSIDE),
            geometry.over_window(boot.BELOW)), (True, False, False))
    expect("a place below the window is more than its height down",
           geometry.relative(boot.BELOW)[1] > 400 * 256, True)
    expect("a place left of the window is less than 0 from its left",
           geometry.relative(boot.OUTSIDE)[0] < 0, True)

    # The window's edges, by hand: the window covers the columns 320 to 959
    # and the rows 120 to 519, those of its left and top edges in and those
    # of its right and bottom edges out. A tablet value across of 8192 is
    # the column 8192 * 1280 / 32768 = 320 exactly, 8191 the column 319,
    # 24576 the column 960 and 24575 the column 959; down, 4916 * 800 /
    # 32768 is 120.02, the row 120, 4915 the row 119, 21300 the row 520 and
    # 21299 the row 519.
    for value, over in ((8191, False), (8192, True), (24575, True),
                        (24576, False)):
        expect("the column the tablet value %d gives is %sthe window's"
               % (value, "" if over else "not "),
               geometry.over_window({"x": value, "y": boot.LAST["y"]}), over)
    for value, over in ((4915, False), (4916, True), (21299, True),
                        (21300, False)):
        expect("the row the tablet value %d gives is %sthe window's"
               % (value, "" if over else "not "),
               geometry.over_window({"x": boot.LAST["x"], "y": value}), over)

    def placed(keys):
        try:
            boot.check_pointer_places(boot.Geometry.of(keys))
        except boot.BootFailed:
            return False
        return True

    for width, height in WINDOW_SCREENS:
        expect("the pointer's places are where the steps want them on a "
               "%dx%d screen" % (width, height),
               placed(window_keys(width, height)), True)
    expect("the places are refused for a window the entry's place is outside",
           placed({"screen": "1280x800", "size": "100x100"}), False)
    expect("the places are refused on a screen where two are one pixel, "
           "though each is where it should be",
           placed({"screen": "8x9", "size": "4x4"}), False)

    # The steps: three screens, the last held, and the lines the window is
    # sent, which are counted by hand: the entry; a motion; the three
    # buttons, each pressed and released; the wheel up and down; the drag
    # of a button out of the window, which is its motion, the press, the
    # motion out and the release, and then the leave; the entry the release
    # of a button pressed away from the window makes; and a last motion.
    steps = boot.pointer_steps(geometry)
    sent = [line for step in steps for line in step.lines]
    used = {tuple(item["data"]["value"] for item in event)
            for step in steps for events in step.events for event in [events]
            if event and event[0]["type"] == "abs"}
    expect("each place the steps move the tablet to is one of the places "
           "the boot keeps out of the log",
           used <= {(at["x"], at["y"]) for at in boot.POINTER_PLACES}, True)
    # The lines for the places, by hand as above: the entry at 77990 and
    # 50548; a motion at the place MOVED, 11971 * 10 - 320 * 256 = 37790 and
    # 9007 * 6.25 - 120 * 256 = 56293 - 30720 = 25573, which a press, a
    # release and a turn of the wheel of 120 are sent at too.
    expect("the entry's line holds the place across and then down",
           (sent[0].event, sent[0].keys["x_256ths"],
            sent[0].keys["y_256ths"]), ("enter", "77990", "50548"))
    expect("a motion's line holds the place across and then down, the wheel "
           "and the buttons",
           (sent[1].keys["x_256ths"], sent[1].keys["y_256ths"],
            sent[1].keys["wheel_120ths"], sent[1].keys["buttons"],
            sent[2].keys["buttons"], sent[8].keys["wheel_120ths"]),
           ("37790", "25573", "0", "none", "left:pressed", "120"))
    expect("the pointer steps match the screen where the pointer is placed, "
           "where it enters and last, held, after the mouse moves",
           [(step.screen, step.hold) for step in steps if step.screen],
           [(boot.OUTSIDE, False), (boot.ENTRY, False), (boot.LAST, True)])
    expect("the window is sent the seventeen lines counted by hand",
           [line.event for line in sent],
           ["enter"] + ["pointer"] * 13 + ["leave", "enter", "pointer"])
    expect("the buttons the window is sent are each of the three pressed and "
           "released, and the drag's",
           [line.keys["buttons"] for line in sent if line.event == "pointer"],
           ["none", "left:pressed", "left:released", "right:pressed",
            "right:released", "middle:pressed", "middle:released", "none",
            "none", "none", "left:pressed", "none", "left:released", "none"])
    expect("the window is sent a turn of one detent up and one down, in "
           "120ths of a detent",
           [line.keys["wheel_120ths"] for line in sent
            if line.event == "pointer"][7:9], ["120", "-120"])

    # A log of the steps' own lines is read in order, and a line the window
    # is not sent, or is sent wrongly, fails the boot.
    pointed, client, client_times = pointer_log(boot, steps)
    first = len(pointed) - len(client)

    def pointing(name, changed, accepted=False):
        expect(name, replay_pointer(boot, steps, changed), accepted)

    pointing("a log of the pointer steps' lines is read in order", pointed,
             True)
    for index, line in enumerate(client):
        position = first + index
        name = "the window boot's pointer with the client's line %d (%s)" % (
            index + 1, line[len("nova-wintest: "):56])
        pointing(name + " lost", pointed[:position] + pointed[position + 1:])
        pointing(name + " twice", pointed[:position + 1] + pointed[position:])
        if index + 1 < len(client):
            swapped = list(pointed)
            swapped[position], swapped[position + 1] = (
                pointed[position + 1], pointed[position])
            pointing(name + " swapped with the next", swapped)
        for field in ("x_256ths", "y_256ths", "wheel_120ths", "buttons"):
            old = [word for word in line.split() if word.startswith(field
                                                                    + "=")]
            if old:
                pointing(name + " with another " + field,
                         pointed[:position]
                         + [line.replace(old[0], field + "=7")]
                         + pointed[position + 1:])
        if " time_ms=" in line:
            pointing(name + " with no time",
                     pointed[:position] + [line[:line.index(" time_ms")]]
                     + pointed[position + 1:])
    pointing("the window boot's pointer with a line after the last",
             pointed + [client[-1]])
    pointing("the window boot's pointer with a time that falls",
             pointed[:first + 2]
             + [pointed[first + 2].replace("time_ms=%.1f" % client_times[2],
                                            "time_ms=0.1")]
             + pointed[first + 3:])
    pointing("the window boot's pointer with the client's lines ahead of the "
             "compositor's", pointed[first:] + pointed[:first], True)
    for index in range(first):
        pointing("the window boot's pointer without the compositor's line "
                 "%d (%s)" % (index + 1,
                              pointed[index][len("nova-compositor: "):]),
                 pointed[:index] + pointed[index + 1:])

    # A script that has read fewer lines than the steps send, or fewer of
    # the compositor's, is not finished.
    for read, name in ((0, "none"), (len(client) - 1, "all but the last")):
        script = boot.PointerScript(steps)
        for line in pointed[:first] + client[:read]:
            script.take(boot.parse_line(line))
        try:
            script.finish()
            finished = True
        except boot.BootFailed:
            finished = False
        expect("a pointer script that has read %s of the client's lines "
               "is not finished" % name, finished, False)

    # The pointer does not move while the keys are typed, so a line the
    # client logs for it then, wherever it comes, fails the boot.
    for line in (client[0], client[1], "nova-wintest: leave"):
        for where in (0, len(lines) // 2, len(lines)):
            keyed("with the client's line %s at %d" % (line[:30], where),
                  lines[:where] + [line] + lines[where:])

    # The window line's keys are kept, and the numbers of the places, which
    # the compositor's lines must not hold, are looked for in the window
    # boot alone; the client's own lines are relative to its window and
    # hold none of them.
    serial = serial_after(boot, WINDOW, [WINDOW_MADE])
    expect("the keys of the client's window line are kept", serial.window,
           {"screen": "1280x800", "size": "640x400"})
    for at in boot.POINTER_PLACES:
        for axis, value in at.items():
            leak = ("nova-compositor: pointer move mode=absolute axes=xy "
                    "%s=%d" % (axis, value))
            expect("the window boot finds the place %s %d in a line of the "
                   "compositor's pointer" % (axis, value),
                   bool(serial_after(boot, WINDOW, [leak]).leaked), True)
            expect("the system boot does not find it, which sends no such "
                   "place", bool(serial_after(boot, "system", [leak]).leaked),
                   False)
    expect("the window boot does not find a place in the client's line",
           serial_after(boot, WINDOW, [
               "nova-wintest: pointer x_256ths=15991 y_256ths=0 "
               "wheel_120ths=0 buttons=none time_ms=1.0"]).leaked, set())

    # A screen is polled for until it is the wanted one, with the caret's
    # two phases as two wanted images, and given up on after the timeout.
    expect("a screen that is the wanted one at once", polled(
        boot, [ppm(7)], [7]) == ([bytes([7]) * (PIXEL_BYTES * SCREEN_WIDTH
                                              * SCREEN_HEIGHT)], 1), True)
    result = polled(boot, [ppm(1), ppm(2), ppm(7)], [7])
    expect("a screen that becomes the wanted one is taken again until it is",
           result and result[1], 3)
    result = polled(boot, [ppm(1), ppm(9)], [7, 9])
    expect("a screen that is the wanted one's other phase is accepted",
           result and result[1], 2)
    expect("a screen that never is the wanted one is given up on",
           polled(boot, [ppm(1)], [7], timeout=0), None)

    # A mode that needs a whole data disk says so, without one.
    for mode in ("fallback", "persist"):
        for extra in ([], ["--data", "data.img", "--corrupt-gpt", "header"]):
            status, message = usage_error(boot, extra + [mode, "disk.img"])
            expect("%s with %s exits with a usage error"
                   % (mode, " ".join(extra) or "no data disk"), status,
                   USAGE_ERROR)
            expect("%s with %s says it needs a whole data disk"
                   % (mode, " ".join(extra) or "no data disk"),
                   "%s needs --data, whole" % mode in message, True)

    # The window boot takes a data disk, whole or not, and none, and
    # takes no fonts.
    for extra in ([], ["--data", "data.img"],
                  ["--data", "data.img", "--corrupt-gpt", "header"]):
        status, message = usage_error(boot, extra + [WINDOW, "window.img"])
        expect("window with %s is no usage error"
               % (" ".join(extra) or "no data disk"), status, None)
    status, message = usage_error(boot, ["--fonts", "fonts", WINDOW,
                                         "window.img"])
    expect("window with fonts exits with a usage error", status, USAGE_ERROR)

    # The modes that run only on a test disk say so, on another.
    for mode in ("chain", "fallback", "specimen", WINDOW):
        status, message = usage_error(boot, ["--data", "data.img", mode,
                                             "disk.img"], tests=False)
        expect("%s without --tests exits with a usage error" % mode, status,
               USAGE_ERROR)
        expect("%s without --tests says it takes only a test disk" % mode,
               "%s takes only a test disk" % mode in message, True)

    for name in failures:
        print("FAILED: " + name)
    print("%d checks, %d failed" % (checks, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
