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
line of the test client, which fails it; the screens a boot waits for,
which poll_screen takes from QMP's screendump until one is the wanted
image or SCREEN_TIMEOUT passes; and the usage errors of a mode that needs
a whole data disk and those of a mode that takes none. Prints each check
that fails, and a count; the exit status is nonzero if any did.
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
            SHOWN % 2]


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
