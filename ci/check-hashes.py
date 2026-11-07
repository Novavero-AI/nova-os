"""Check the files README.md gives a size and sha256 for.

Usage: python3 -I ci/check-hashes.py README DIR

Every table row of the form | `NAME` | SIZE | `SHA256` | names a file
under DIR, and the sentence giving the TinyCC archive's sha256 names
DIR/tinycc.tar.gz, which has no size in the README. Each file is read,
and one line per file says whether it matches. The exit status is
nonzero if any file is missing or differs, if a row that starts like
one of these is not in that form, or if the README holds no row or no
archive hash, so that a change to the README's wording cannot leave
files unchecked.
"""

import hashlib
import pathlib
import re
import sys

ROW = re.compile(r"\| `([^`]+)` \| ([0-9,]+) \| `([0-9a-f]{64})` \|")
ROW_START = "| `"
ARCHIVE = re.compile(r"archive's sha256 is\s+`([0-9a-f]{64})`")
ARCHIVE_FILE = "tinycc.tar.gz"


def expected_files(readme):
    """The (name, size, sha256) the README gives, size None for the archive."""
    rows = []
    for line in readme.splitlines():
        if not line.startswith(ROW_START):
            continue
        match = ROW.fullmatch(line)
        if not match:
            sys.exit("README row not in the form | `NAME` | SIZE | `SHA256` |: " + line)
        name, size, digest = match.groups()
        rows.append((name, int(size.replace(",", "")), digest))
    if not rows:
        sys.exit("README holds no | `NAME` | SIZE | `SHA256` | row")
    archive = ARCHIVE.search(readme)
    if not archive:
        sys.exit("README gives no sha256 for the TinyCC archive")
    return [(ARCHIVE_FILE, None, archive.group(1))] + rows


def check(directory, name, size, digest):
    """One line saying whether directory/name has the size and sha256 given."""
    path = directory / name
    if not path.is_file():
        return False, "MISSING   %s" % name
    data = path.read_bytes()
    built = hashlib.sha256(data).hexdigest()
    if built == digest and size in (None, len(data)):
        return True, "ok        %s" % name
    wanted = digest if size is None else "%d bytes, %s" % (size, digest)
    return False, "MISMATCH  %s: README %s, built %d bytes, %s" % (
        name, wanted, len(data), built)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: check-hashes.py README DIR")
    readme = pathlib.Path(sys.argv[1]).read_text(encoding="ascii")
    directory = pathlib.Path(sys.argv[2])
    results = [check(directory, *row) for row in expected_files(readme)]
    for _, line in results:
        print(line)
    matched = sum(1 for ok, _ in results if ok)
    print("%d of %d files match README.md" % (matched, len(results)))
    return 0 if matched == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
