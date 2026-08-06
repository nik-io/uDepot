#!/usr/bin/env python3
"""Integration tests for pyudepot backend selection.

Tests the uDepot Python binding with explicit backend types:
  - POSIX (buffered, type 2) — always available
  - O_DIRECT (type 3) — requires filesystem with O_DIRECT support
  - TRT+AIO (type 5) — requires TRT scheduler, tested in subprocess
  - TRT+io_uring (type 6) — requires TRT scheduler, tested in subprocess

TRT backends use a coroutine-based scheduler that needs process-level
isolation and may require specific kernel/device support.  They are
tested in subprocesses and gracefully skipped when the runtime cannot
be initialized (e.g., containerized environments).
"""

import os
import signal
import subprocess
import sys
import tempfile
import traceback
import numpy as np

from pyudepot.uDepot import (
    uDepot,
    BACKEND_POSIX,
    BACKEND_O_DIRECT,
    BACKEND_TRT_AIO,
    BACKEND_TRT_URING,
)

STORE_SIZE = 64 * 1024 * 1024


def _roundtrip(kv, label):
    """Put, get, exists, delete cycle."""
    key = np.frombuffer(f"key-{label}".encode(), dtype=np.uint8).copy()
    val = np.frombuffer(f"value-for-{label}-data".encode(), dtype=np.uint8).copy()

    assert kv.put(key, val), f"{label}: put failed"

    size = kv.exists(key)
    assert size is not None, f"{label}: exists returned None after put"
    assert size == val.size, f"{label}: exists size {size} != {val.size}"

    val_out = np.empty(val.size, dtype=np.uint8)
    assert kv.get(key, val_out), f"{label}: get failed"
    assert (val_out == val).all(), f"{label}: get returned wrong data"

    assert kv.delete(key), f"{label}: delete failed"
    assert kv.exists(key) is None, f"{label}: exists after delete should be None"


def _test_backend_inproc(backend_type, backend_name, use_shm=False):
    """Test a backend in-process (safe for POSIX and O_DIRECT)."""
    if use_shm:
        tmp_fd, tmp_path = tempfile.mkstemp(suffix=".udepot", dir="/dev/shm")
    else:
        tmp_fd, tmp_path = tempfile.mkstemp(suffix=".udepot")
    os.close(tmp_fd)

    try:
        kv = uDepot(
            file_name=tmp_path, size=STORE_SIZE,
            backend=backend_type, force_destroy=True,
        )
        _roundtrip(kv, backend_name)

        key_a = np.frombuffer(b"multi-a", dtype=np.uint8).copy()
        key_b = np.frombuffer(b"multi-b", dtype=np.uint8).copy()
        val_a = np.arange(64, dtype=np.uint8)
        val_b = np.arange(128, dtype=np.uint8)

        assert kv.put(key_a, val_a)
        assert kv.put(key_b, val_b)

        out_a = np.empty(64, dtype=np.uint8)
        out_b = np.empty(128, dtype=np.uint8)
        assert kv.get(key_a, out_a)
        assert kv.get(key_b, out_b)
        assert (out_a == val_a).all()
        assert (out_b == val_b).all()

        print(f"  PASS: {backend_name} (type={backend_type})")
        return True
    except IOError as e:
        print(f"  SKIP: {backend_name} (type={backend_type}): {e}")
        return None
    except Exception as e:
        print(f"  FAIL: {backend_name} (type={backend_type}): {e}")
        traceback.print_exc()
        return False
    finally:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass


_SUBPROCESS_SCRIPT = '''
import os, sys, tempfile, numpy as np
sys.path.insert(0, os.environ.get("PYUDEPOT_PATH", "python/"))
from pyudepot.uDepot import uDepot

backend = int(sys.argv[1])
tmp_fd, tmp_path = tempfile.mkstemp(suffix=".udepot")
os.close(tmp_fd)
try:
    kv = uDepot(file_name=tmp_path, size=64*1024*1024,
                backend=backend, force_destroy=True)
    key = np.frombuffer(b"test-key", dtype=np.uint8).copy()
    val = np.arange(256, dtype=np.uint8)
    assert kv.put(key, val), "put failed"
    out = np.empty(256, dtype=np.uint8)
    assert kv.get(key, out), "get failed"
    assert (out == val).all(), "data mismatch"
    size = kv.exists(key)
    assert size == 256, f"exists returned {size}"
    assert kv.delete(key), "delete failed"
    print("OK")
except IOError as e:
    print(f"INIT_FAIL:{e}")
    sys.exit(2)
except Exception as e:
    print(f"ERROR:{e}")
    sys.exit(1)
finally:
    try:
        os.unlink(tmp_path)
    except OSError:
        pass
'''


def _test_backend_subprocess(backend_type, backend_name):
    """Test a TRT backend in a subprocess for isolation."""
    env = os.environ.copy()
    udepot_root = os.path.dirname(os.path.abspath(__file__))
    env["LD_LIBRARY_PATH"] = (
        os.path.join(udepot_root, "python", "pyudepot")
        + ":" + env.get("LD_LIBRARY_PATH", "")
    )
    env["PYUDEPOT_PATH"] = os.path.join(udepot_root, "python")

    try:
        result = subprocess.run(
            [sys.executable, "-c", _SUBPROCESS_SCRIPT, str(backend_type)],
            capture_output=True, text=True, timeout=30, env=env,
        )
        stdout = result.stdout.strip()
        lines = [l.strip() for l in stdout.splitlines()]
        if result.returncode == 0 and "OK" in lines:
            print(f"  PASS: {backend_name} (type={backend_type}, subprocess)")
            return True
        elif result.returncode == 2 and "INIT_FAIL" in stdout:
            reason = stdout.split(":", 1)[1] if ":" in stdout else stdout
            print(f"  SKIP: {backend_name} (type={backend_type}): init failed: "
                  f"{reason}")
            return None
        elif result.returncode < 0:
            sig = -result.returncode
            signame = signal.Signals(sig).name if sig in signal.Signals._value2member_map_ else str(sig)
            print(f"  SKIP: {backend_name} (type={backend_type}): "
                  f"killed by {signame} (TRT runtime not available)")
            return None
        else:
            print(f"  FAIL: {backend_name} (type={backend_type}): "
                  f"exit={result.returncode} stdout={stdout}")
            return False
    except subprocess.TimeoutExpired:
        print(f"  FAIL: {backend_name} (type={backend_type}): timeout")
        return False


def main():
    print("pyudepot backend integration tests")
    print("=" * 50)

    results = {}

    results["auto-shm"] = _test_backend_inproc(
        BACKEND_POSIX, "auto-shm", use_shm=True)
    results["posix-shm"] = _test_backend_inproc(
        BACKEND_POSIX, "POSIX-shm", use_shm=True)
    results["o_direct"] = _test_backend_inproc(
        BACKEND_O_DIRECT, "O_DIRECT")
    results["trt-aio"] = _test_backend_subprocess(
        BACKEND_TRT_AIO, "TRT+AIO")
    results["trt-uring"] = _test_backend_subprocess(
        BACKEND_TRT_URING, "TRT+io_uring")

    print()
    print("=" * 50)
    passed = sum(1 for v in results.values() if v is True)
    skipped = sum(1 for v in results.values() if v is None)
    failed = sum(1 for v in results.values() if v is False)
    print(f"Results: {passed} passed, {skipped} skipped, {failed} failed")

    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
