"""Generate nova-os's API reference and check that it documents every
public declaration.

Usage: python3 -I ci/reference.py OUT    (DOXYGEN names the program,
                                          doxygen by default)

Runs Doxygen with the repository's Doxyfile at its root, writing the
reference to OUT/html, a new directory, and Doxygen's XML beside it. The
check fails if Doxygen gives any warning, which the Doxyfile's
WARN_AS_ERROR turns into a nonzero exit; if a header Doxygen read has no
file comment, since Doxygen then leaves the header's macros and
functions out of the reference without a warning; or if a page holds the
checkout's absolute path.

It then checks that the check bites: Doxygen runs again with the same
settings over copies of four headers, each changed as a change to
nova-os could leave it, and must fail, naming each change. A function
loses its comment (WARN_IF_UNDOCUMENTED), and a static inline function
(EXTRACT_STATIC) and an enum value (WARN_IF_UNDOC_ENUM_VAL) theirs; a
header loses its file comment, which only the file comment check finds.
"""

import os
import pathlib
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

DOXYFILE = "Doxyfile"
HEADER_SUFFIX = ".h"
FILE_COMMENT = "/** @file"

# Each probe: a header, the declaration whose comment it loses, and the
# Doxyfile setting whose warning names it.
PROBES = [
    ("native/native.h", "handle_close", "WARN_IF_UNDOCUMENTED"),
    ("include/memory_layout.h", "direct_map", "EXTRACT_STATIC"),
    ("lib/console.h", "CONSOLE_NO_ECHO", "WARN_IF_UNDOC_ENUM_VAL"),
]
# A header that loses its file comment.
FILE_PROBE = "lib/log.h"


class CheckFailed(Exception):
    pass


def run_doxygen(doxygen, root, overrides):
    """Run Doxygen at root with the Doxyfile and the settings in overrides
    after it, which take their place; its exit status and its warnings."""
    config = (root / DOXYFILE).read_text(encoding="ascii")
    config += "".join('%s = "%s"\n' % item for item in overrides.items())
    result = subprocess.run([doxygen, "-"], cwd=root, input=config,
                            capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    sys.stdout.flush()
    return result.returncode, result.stderr


def undocumented_headers(xml):
    """The headers in Doxygen's XML index that have no file comment."""
    index = ET.parse(xml / "index.xml").getroot()
    headers = [compound for compound in index.findall("compound")
               if compound.get("kind") == "file"
               and compound.findtext("name").endswith(HEADER_SUFFIX)]
    if not headers:
        raise CheckFailed("Doxygen read no header")
    missing = []
    for header in headers:
        definition = ET.parse(xml / (header.get("refid") + ".xml")).find(
            "compounddef")
        text = "".join(definition.find("briefdescription").itertext())
        text += "".join(definition.find("detaileddescription").itertext())
        if not text.strip():
            missing.append(header.findtext("name"))
    return missing


def generate(doxygen, root, out):
    """Generate the reference into out and check it."""
    status, warnings = run_doxygen(doxygen, root, {
        "OUTPUT_DIRECTORY": out,
        "GENERATE_XML": "YES",
    })
    sys.stdout.write(warnings)
    if status != 0:
        raise CheckFailed("Doxygen gave the warnings above")
    missing = undocumented_headers(out / "xml")
    if missing:
        raise CheckFailed("no file comment, so Doxygen leaves their "
                          "declarations out unwarned: " + ", ".join(missing))
    absolute = str(root).encode()
    for page in (out / "html").rglob("*"):
        if page.is_file() and absolute in page.read_bytes():
            raise CheckFailed("%s holds the absolute path %s"
                              % (page.relative_to(out), root))
    print("every public declaration is documented")


def without_comment(text, name):
    """text with the comment of the declaration of name removed: one on
    the line before it, other than a group's opening or closing, or one
    after it on its line."""
    before = re.compile(r"^[ \t]*/\*\*(?!<)(?!\s*@)(?:(?!\*/).)*\*/\n"
                        r"(?=[^\n]*\b%s\b)" % name, re.S | re.M)
    after = re.compile(r"(\b%s\b[^\n]*?)\s*/\*\*<[^\n]*?\*/" % name)
    text, removed = before.subn("", text)
    if not removed:
        text, removed = after.subn(r"\1", text)
    if removed != 1:
        raise CheckFailed("the probe found %d comments of %s"
                          % (removed, name))
    return text


def probe(doxygen, root):
    """Check that Doxygen and the file comment check fail on the probes."""
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        headers = scratch / "headers"
        headers.mkdir()
        for path, name, _ in PROBES:
            text = (root / path).read_text(encoding="ascii")
            (headers / pathlib.Path(path).name).write_text(
                without_comment(text, name), encoding="ascii")
        text = (root / FILE_PROBE).read_text(encoding="ascii")
        if text.count(FILE_COMMENT) != 1:
            raise CheckFailed("%s has no one file comment to remove"
                              % FILE_PROBE)
        (headers / pathlib.Path(FILE_PROBE).name).write_text(
            text.replace(FILE_COMMENT, "/*"), encoding="ascii")
        status, warnings = run_doxygen(doxygen, root, {
            "INPUT": headers,
            "USE_MDFILE_AS_MAINPAGE": "",
            "OUTPUT_DIRECTORY": scratch / "out",
            "GENERATE_HTML": "NO",
            "GENERATE_XML": "YES",
        })
        failed = []
        if status == 0:
            failed.append("Doxygen passed the probes")
        for path, name, setting in PROBES:
            if not re.search(r"\b%s\b" % name, warnings):
                failed.append("no warning names %s, which lost its comment "
                              "in %s (%s)" % (name, path, setting))
        missing = undocumented_headers(scratch / "out" / "xml")
        if missing != [pathlib.Path(FILE_PROBE).name]:
            failed.append("the file comment check found %s, not %s"
                          % (missing, FILE_PROBE))
        if failed:
            sys.stdout.write(warnings)
            raise CheckFailed("the check does not bite:\n  "
                              + "\n  ".join(failed))
    print("the check fails on each probe: %s, and %s's file comment"
          % (", ".join(name for _, name, _ in PROBES), FILE_PROBE))


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: reference.py OUT")
    out = pathlib.Path(sys.argv[1]).resolve()
    try:
        out.mkdir(parents=True)
    except FileExistsError:
        sys.exit("%s exists; OUT is a new directory" % out)
    ci = pathlib.Path(__file__).resolve().parent
    root = pathlib.Path(subprocess.run(
        ["git", "-C", ci, "rev-parse", "--show-toplevel"], check=True,
        capture_output=True, text=True).stdout.strip())
    doxygen = os.environ.get("DOXYGEN", "doxygen")
    subprocess.run([doxygen, "--version"], check=True)
    try:
        generate(doxygen, root, out)
        probe(doxygen, root)
    except CheckFailed as failure:
        print("FAILED: %s" % failure, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
