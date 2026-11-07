"""Check the files SHA256SUMS gives a sha256 for.

Usage: python3 -I ci/check-hashes.py SHA256SUMS DIR

Every line of SHA256SUMS is a sha256 in lowercase hexadecimal, two
spaces and a file's path relative to DIR, as sha256sum writes them.
Each file is read, and one line per file says whether it matches. The
exit status is nonzero if any file is missing or differs, if a line is
not in that form, or if SHA256SUMS names no file, so that no line is
passed over unchecked.
"""

import hashlib
import pathlib
import re
import sys

LINE = re.compile(r"([0-9a-f]{64})  (.+)")


def expected_files(sums):
    """The (name, sha256) of each line of sums, the file's text."""
    files = []
    for line in sums.splitlines():
        match = LINE.fullmatch(line)
        if not match:
            sys.exit("SHA256SUMS line not in the form SHA256  NAME: " + line)
        digest, name = match.groups()
        files.append((name, digest))
    if not files:
        sys.exit("SHA256SUMS names no file")
    return files


def check(directory, name, digest):
    """One line saying whether directory/name has the sha256 given."""
    path = directory / name
    if not path.is_file():
        return False, "MISSING   %s" % name
    data = path.read_bytes()
    built = hashlib.sha256(data).hexdigest()
    if built == digest:
        return True, "ok        %s" % name
    return False, "MISMATCH  %s: SHA256SUMS %s, built %d bytes, %s" % (
        name, digest, len(data), built)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: check-hashes.py SHA256SUMS DIR")
    sums = pathlib.Path(sys.argv[1])
    directory = pathlib.Path(sys.argv[2])
    results = [check(directory, *expected)
               for expected in expected_files(sums.read_text(encoding="ascii"))]
    for _, line in results:
        print(line)
    matched = sum(1 for ok, _ in results if ok)
    print("%d of %d files match %s" % (matched, len(results), sums.name))
    return 0 if matched == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
