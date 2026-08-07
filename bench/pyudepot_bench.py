#!/usr/bin/env python3
"""Throughput benchmark for the pyudepot ctypes bindings.

Measures the Python-facing KV path: numpy buffers in, numpy buffers out,
through ctypes into libpyudepot. This is the binding overhead plus uDepot,
which is what a Python caller actually sees -- distinct from the C++ I/O
layer benchmarks, and the reason it lives beside them rather than in a
downstream project.

Output matches the other benchmarks so one parser handles them all:

    PUT time=1.234567s Kops/sec=12.345 MiB/sec=48.20
    GET time=1.234567s Kops/sec=12.345 MiB/sec=48.20

Usage:
    python3 bench/pyudepot_bench.py [--backend N] [-n OPS] [--val-size BYTES]
                                    [-f FILE]
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

import numpy as np

# Prefer the in-tree package so the benchmark runs from a build tree.
_REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_REPO_ROOT / "python"))

import pyudepot  # noqa: E402

DEFAULT_OPS = 20000
DEFAULT_VAL_SIZE = 3072
DEFAULT_SIZE = (1048576 + 4096) * 1024 + 1

# Backends that do not need SPDK or a special device.
BACKEND_NAMES = {
    2: "posix",
    3: "o_direct",
    5: "trt_aio",
    6: "trt_uring",
}


def _keys(count: int) -> np.ndarray:
    """Build a deterministic key table as a (count, 8) uint8 array."""
    idx = np.arange(count, dtype=np.uint64)
    # Same mixing constant the C++ thin tests use, so key distribution matches.
    mixed = (idx + 1) * np.uint64(2654435761)
    return mixed.view(np.uint8).reshape(count, 8).copy()


def report(phase: str, secs: float, ops: int, val_size: int) -> None:
    """Print one result line in the shared benchmark format."""
    print(
        f"{phase} time={secs:.6f}s "
        f"Kops/sec={ops / (secs * 1000):.6f} "
        f"MiB/sec={(ops * val_size) / (secs * 1024 * 1024):.6f}",
        flush=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", type=int, default=3,
                        help="uDepot backend id (default: 3, O_DIRECT)")
    parser.add_argument("-n", "--ops", type=int, default=DEFAULT_OPS,
                        help=f"operations per phase (default: {DEFAULT_OPS})")
    parser.add_argument("--val-size", type=int, default=DEFAULT_VAL_SIZE,
                        help=f"value size in bytes (default: {DEFAULT_VAL_SIZE})")
    parser.add_argument("-f", "--file", default="/tmp/pyudepot-bench.udepot",
                        help="store path")
    parser.add_argument("--size", type=int, default=DEFAULT_SIZE,
                        help="store size in bytes")
    args = parser.parse_args()

    if os.path.exists(args.file):
        os.unlink(args.file)

    backend_name = BACKEND_NAMES.get(args.backend, str(args.backend))
    print(f"f:{args.file} backend:{backend_name} ops:{args.ops} "
          f"val_size:{args.val_size}", flush=True)

    kv = pyudepot.uDepot(
        file_name=args.file, size=args.size,
        backend=args.backend, force_destroy=True,
    )

    keys = _keys(args.ops)
    val = np.arange(args.val_size, dtype=np.uint8)

    start = time.monotonic()
    for i in range(args.ops):
        if not kv.put(keys[i], val):
            print(f"put failed at {i}", file=sys.stderr)
            return 1
    report("PUT", time.monotonic() - start, args.ops, args.val_size)

    out = np.zeros(args.val_size, dtype=np.uint8)
    start = time.monotonic()
    for i in range(args.ops):
        if not kv.get(keys[i], out):
            print(f"get failed at {i}", file=sys.stderr)
            return 1
    report("GET", time.monotonic() - start, args.ops, args.val_size)

    if os.path.exists(args.file):
        os.unlink(args.file)
    return 0


if __name__ == "__main__":
    sys.exit(main())
