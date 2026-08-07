#!/usr/bin/env python3
"""Build test for the pyudepot bindings.

Verifies that libpyudepot.so builds, imports, and round-trips a key/value
through the real library. That is all it does.

There is deliberately no performance assertion here. The only performance
property worth asserting for these bindings is the zero-copy one, and that is a
comparison of the same operation with and without zero copy -- not a comparison
across different operations. An absolute latency bound, or a rule that GET must
beat PUT, says more about the machine it ran on than about the code: which of
the two is faster depends on the backend, the page cache, and the device.

Run via: make run_pyudepot_build_test
"""

from __future__ import annotations

import os
import sys
import tempfile

import numpy as np

import pyudepot

STORE_SIZE = 64 * 1024 * 1024
BACKEND_O_DIRECT = 3
VAL_SIZE = 256


def main() -> int:
    fd, path = tempfile.mkstemp(prefix="pyudepot-build-test-", suffix=".udepot")
    os.close(fd)
    os.unlink(path)  # uDepot creates it; we only wanted a unique name

    try:
        kv = pyudepot.uDepot(
            file_name=path, size=STORE_SIZE,
            backend=BACKEND_O_DIRECT, force_destroy=True,
        )

        key = np.frombuffer(b"build-test".ljust(16, b"\0"), dtype=np.uint8)
        val = np.arange(VAL_SIZE, dtype=np.uint8)

        if not kv.put(key, val):
            print("put failed", file=sys.stderr)
            return 1

        out = np.zeros(VAL_SIZE, dtype=np.uint8)
        if not kv.get(key, out):
            print("get failed", file=sys.stderr)
            return 1

        if not np.array_equal(out, val):
            print("value mismatch after round trip", file=sys.stderr)
            return 1

        print("pyudepot build test OK")
        return 0
    finally:
        if os.path.exists(path):
            os.unlink(path)


if __name__ == "__main__":
    sys.exit(main())
