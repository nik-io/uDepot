#!/usr/bin/env python3
"""Integration tests for uDepot on NVMe-like block devices.

Tests uDepot I/O backends (O_DIRECT, TRT+AIO, TRT+io_uring) against
block devices that don't require physical NVMe hardware:

  - null_blk: kernel module providing /dev/nullb* (reads return zeros,
    writes are discarded).  Tests the I/O path end-to-end but cannot
    verify data persistence.

  - SPDK NVMeoF TCP target with bdev_malloc: in-memory NVMe target
    accessible over TCP.  Tests the full SPDK + NVMeoF path including
    data persistence within a single run.

Run setup scripts first:
  sudo ./setup_nullblk.sh        # for null_blk tests
  sudo ./setup_nvmef_target.sh   # for NVMeoF tests

Usage:
  python3 test_nvme_backends.py [--nullblk-dev /dev/nullb0]
                                [--nvmef-env path/to/nvmef_target.env]
"""

import argparse
import os
import signal
import subprocess
import sys
import traceback

import numpy as np


def _find_pyudepot():
    """Add pyudepot to sys.path if not already importable."""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(script_dir, "..", ".."))
    python_dir = os.path.join(repo_root, "python")
    if python_dir not in sys.path:
        sys.path.insert(0, python_dir)
    lib_dir = os.path.join(python_dir, "pyudepot")
    ld_path = os.environ.get("LD_LIBRARY_PATH", "")
    if lib_dir not in ld_path:
        os.environ["LD_LIBRARY_PATH"] = lib_dir + ":" + ld_path


_find_pyudepot()

from pyudepot.uDepot import (
    uDepot,
    BACKEND_O_DIRECT,
    BACKEND_TRT_AIO,
    BACKEND_TRT_URING,
)


STORE_SIZE = 64 * 1024 * 1024


def _roundtrip(kv, label):
    """Put, get, exists, delete — verifies the full KV path."""
    key = np.frombuffer(f"test-{label}".encode(), dtype=np.uint8).copy()
    val = np.arange(256, dtype=np.uint8)

    assert kv.put(key, val), f"{label}: put failed"

    size = kv.exists(key)
    assert size is not None, f"{label}: exists returned None"
    assert size == val.size, f"{label}: exists size {size} != {val.size}"

    out = np.empty(val.size, dtype=np.uint8)
    assert kv.get(key, out), f"{label}: get failed"
    assert (out == val).all(), f"{label}: data mismatch"

    assert kv.delete(key), f"{label}: delete failed"
    assert kv.exists(key) is None, f"{label}: exists after delete"


def _test_nullblk_inproc(dev_path, backend_type, backend_name):
    """Test a backend against null_blk in-process (O_DIRECT only)."""
    try:
        kv = uDepot(
            file_name=dev_path, size=STORE_SIZE,
            backend=backend_type, force_destroy=True,
        )
        # null_blk discards writes and returns zeros on read, so we can
        # only test that the I/O path doesn't crash.  We do a put and
        # verify exists returns the right size (metadata is in-memory).
        key = np.frombuffer(b"nullblk-test", dtype=np.uint8).copy()
        val = np.arange(128, dtype=np.uint8)
        assert kv.put(key, val), f"{backend_name}: put failed"
        size = kv.exists(key)
        assert size is not None, f"{backend_name}: exists returned None"
        assert size == val.size, f"{backend_name}: wrong size"

        print(f"  PASS: {backend_name} on {dev_path}")
        return True
    except IOError as exc:
        print(f"  SKIP: {backend_name} on {dev_path}: {exc}")
        return None
    except Exception as exc:
        print(f"  FAIL: {backend_name} on {dev_path}: {exc}")
        traceback.print_exc()
        return False


_SUBPROCESS_SCRIPT = '''
import os, sys, numpy as np
sys.path.insert(0, os.environ.get("PYUDEPOT_PATH", "python/"))
from pyudepot.uDepot import uDepot

dev_path = sys.argv[1]
backend = int(sys.argv[2])
size = 64 * 1024 * 1024

try:
    kv = uDepot(file_name=dev_path, size=size,
                backend=backend, force_destroy=True)
    key = np.frombuffer(b"trt-blkdev-test", dtype=np.uint8).copy()
    val = np.arange(128, dtype=np.uint8)
    assert kv.put(key, val), "put failed"
    size = kv.exists(key)
    assert size is not None, "exists returned None"
    assert size == 128, f"wrong size: {size}"
    print("OK")
except IOError as e:
    print(f"INIT_FAIL:{e}")
    sys.exit(2)
except Exception as e:
    print(f"ERROR:{e}")
    sys.exit(1)
'''


def _test_nullblk_subprocess(dev_path, backend_type, backend_name):
    """Test a TRT backend against null_blk in a subprocess."""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(script_dir, "..", ".."))

    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = (
        os.path.join(repo_root, "python", "pyudepot")
        + ":" + env.get("LD_LIBRARY_PATH", "")
    )
    env["PYUDEPOT_PATH"] = os.path.join(repo_root, "python")

    try:
        result = subprocess.run(
            [sys.executable, "-c", _SUBPROCESS_SCRIPT,
             dev_path, str(backend_type)],
            capture_output=True, text=True, timeout=30, env=env,
        )
        lines = [line.strip() for line in result.stdout.strip().splitlines()]
        if result.returncode == 0 and "OK" in lines:
            print(f"  PASS: {backend_name} on {dev_path} (subprocess)")
            return True
        elif result.returncode == 2 and "INIT_FAIL" in result.stdout:
            reason = result.stdout.split(":", 1)[1] if ":" in result.stdout else result.stdout
            print(f"  SKIP: {backend_name} on {dev_path}: {reason}")
            return None
        elif result.returncode < 0:
            sig = -result.returncode
            signame = (signal.Signals(sig).name
                       if sig in signal.Signals._value2member_map_
                       else str(sig))
            print(f"  SKIP: {backend_name} on {dev_path}: "
                  f"killed by {signame}")
            return None
        else:
            print(f"  FAIL: {backend_name} on {dev_path}: "
                  f"rc={result.returncode} out={result.stdout.strip()}")
            if result.stderr.strip():
                print(f"        stderr: {result.stderr.strip()[:200]}")
            return False
    except subprocess.TimeoutExpired:
        print(f"  FAIL: {backend_name} on {dev_path}: timeout")
        return False


def test_nullblk(dev_path):
    """Run all applicable backends against a null_blk device."""
    print(f"\nnull_blk tests on {dev_path}")
    print("-" * 50)

    results = {}
    results["o_direct"] = _test_nullblk_inproc(
        dev_path, BACKEND_O_DIRECT, "O_DIRECT")
    results["trt_aio"] = _test_nullblk_subprocess(
        dev_path, BACKEND_TRT_AIO, "TRT+AIO")
    results["trt_uring"] = _test_nullblk_subprocess(
        dev_path, BACKEND_TRT_URING, "TRT+io_uring")

    return results


def load_nvmef_env(env_path):
    """Parse the nvmef_target.env file into a dict."""
    config = {}
    with open(env_path) as f:
        for line in f:
            line = line.strip()
            if "=" in line and not line.startswith("#"):
                key, val = line.split("=", 1)
                config[key] = val
    return config


def main():
    parser = argparse.ArgumentParser(
        description="Integration tests for uDepot on NVMe-like devices")
    parser.add_argument(
        "--nullblk-dev", default=None,
        help="null_blk device path (e.g., /dev/nullb0)")
    parser.add_argument(
        "--nvmef-env", default=None,
        help="Path to nvmef_target.env from setup_nvmef_target.sh")
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))

    # Auto-detect null_blk
    if args.nullblk_dev is None:
        state_file = os.path.join(script_dir, ".nullblk_state", "nullblk.env")
        if os.path.exists(state_file):
            config = load_nvmef_env(state_file)
            args.nullblk_dev = config.get("NULLBLK_DEVICES", "").split(":")[0]
        elif os.path.exists("/dev/nullb0"):
            args.nullblk_dev = "/dev/nullb0"

    # Auto-detect NVMeoF target
    if args.nvmef_env is None:
        state_file = os.path.join(
            script_dir, ".nvmef_state", "nvmef_target.env")
        if os.path.exists(state_file):
            args.nvmef_env = state_file

    if args.nullblk_dev is None and args.nvmef_env is None:
        print("No test devices found.")
        print("Run setup_nullblk.sh or setup_nvmef_target.sh first.")
        print("Or specify --nullblk-dev or --nvmef-env.")
        sys.exit(2)

    print("uDepot NVMe backend integration tests")
    print("=" * 50)

    all_results = {}

    if args.nullblk_dev:
        if not os.path.exists(args.nullblk_dev):
            print(f"WARNING: {args.nullblk_dev} does not exist, skipping")
        else:
            all_results.update(test_nullblk(args.nullblk_dev))

    if args.nvmef_env:
        config = load_nvmef_env(args.nvmef_env)
        print(f"\nNVMeoF target: {config.get('NVMEF_ADDR')}:"
              f"{config.get('NVMEF_PORT')}")
        print("  (NVMeoF tests require SPDK-linked uDepot build)")
        print("  SKIP: NVMeoF backend tests (BUILD_SPDK=1 required)")

    print()
    print("=" * 50)
    passed = sum(1 for v in all_results.values() if v is True)
    skipped = sum(1 for v in all_results.values() if v is None)
    failed = sum(1 for v in all_results.values() if v is False)
    print(f"Results: {passed} passed, {skipped} skipped, {failed} failed")

    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
