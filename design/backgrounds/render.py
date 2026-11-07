"""Render one background candidate at any size to a binary PPM, through
its own shade and GRAIN and common.finish, for comparison with a C port.

    python3 render.py NAME WIDTH HEIGHT OUT.ppm
"""

import importlib
import multiprocessing
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROWS_PER_TASK = 16
USAGE_ERROR = 2

_candidate = None
_finish = None
_width = 0


def _load(name, width, height):
    """Size common before importing the candidate, which reads W and H
    once, when it is imported."""
    global _candidate, _finish, _width
    sys.path.insert(0, HERE)
    common = importlib.import_module("common")
    common.W, common.H = width, height
    _candidate = importlib.import_module(name)
    _finish = common.finish
    _width = width


def _rows(span):
    out = bytearray()
    for y in range(*span):
        for x in range(_width):
            out.extend(_finish(x, y, _candidate.shade(x, y), _candidate.GRAIN))
    return bytes(out)


def main():
    if len(sys.argv) != 5:
        print("usage: render.py NAME WIDTH HEIGHT OUT.ppm", file=sys.stderr)
        sys.exit(USAGE_ERROR)
    name, path = sys.argv[1], sys.argv[4]
    width, height = int(sys.argv[2]), int(sys.argv[3])
    # A candidate that fails to import fails here, once: in a pool's
    # initializer the failure would only start another worker.
    _load(name, width, height)
    spans = [(y, min(y + ROWS_PER_TASK, height)) for y in range(0, height, ROWS_PER_TASK)]
    with multiprocessing.get_context("spawn").Pool(
        initializer=_load, initargs=(name, width, height)
    ) as pool:
        rows = pool.map(_rows, spans)
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (width, height))
        for chunk in rows:
            f.write(chunk)


if __name__ == "__main__":
    main()
