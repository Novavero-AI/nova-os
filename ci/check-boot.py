"""Check which lines ci/boot.py's fallback boot requires and accepts.

Usage: python3 -I ci/check-boot.py

Replays serial lines through boot.py's Serial, as a boot reads them, and
checks what boot.py makes of them: the lines docs/spec/display.md D19's
rule 9 has the fallback boot report, each as often as it gives, and the
same boot with the input drivers' ends read before the compositor's; the
failure lines the fallback boot accepts, each as often as it gives and
each alone, and no failure line in any other mode; the console's display
lines of the other boots, which a display after the compositor is not;
and the usage errors of a mode that needs a whole data disk. Prints each
check that fails, and a count; the exit status is nonzero if any did.
"""

import contextlib
import importlib.util
import io
import os
import pathlib
import sys
import time
import unittest.mock

BOOT_PATH = pathlib.Path(__file__).with_name("boot.py")
REPLAY_SECONDS = 10
FUNCTIONS = ["0000:00:03.0", "0000:00:04.0", "0000:00:05.0"]
OTHER_FUNCTION = "0000:00:06.0"
FALLBACK = "fallback"
SENTINEL = "nova: startup done"
USAGE_ERROR = 2

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


def failure_lines(log):
    """The lines of log that report a failure, which the fallback boot
    accepts."""
    return [line for line in log
            if "child restarted" in line or "child given up" in line]


def replay(boot, mode, lines, wanted=()):
    """Whether Serial reads lines, then the sentinel, and finds each
    Event in wanted reported as often as it must be, without failing the
    boot, which accepts the failures of mode."""
    stream = [(line + "\r\n").encode("ascii") for line in lines + [SENTINEL]]
    serial = boot.Serial(stream, io.BytesIO(), None, None,
                         boot.Accepted(boot.accepted_failures(mode)))
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            serial.wait_for(list(wanted) + [boot.Event(SENTINEL)],
                            time.monotonic() + REPLAY_SECONDS)
    except boot.BootFailed:
        return False
    finally:
        serial.reader.join()
    return True


def usage_error(boot, arguments):
    """The status and the message boot.main exits with for arguments,
    or None for the status where it goes on to a boot, which is not made
    here."""
    command = ["boot.py", "--code", "code.fd", "--vars", "vars.fd",
               "--log", os.devnull, "--tests"] + arguments
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

    for name in failures:
        print("FAILED: " + name)
    print("%d checks, %d failed" % (checks, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
