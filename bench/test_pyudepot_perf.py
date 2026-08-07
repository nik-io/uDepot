"""Performance tests for the pyudepot ctypes bindings.

Scope: the Python-facing KV path only. This is the one perf suite in uDepot
written in Python, because the thing under test is the Python binding layer --
numpy buffers through ctypes into libpyudepot. The C++ layers have C++/shell
suites instead:

    raw I/O backends       trt/bench/perf_test.sh
    uDepot KV interfaces   bench/perf_test.sh

Both checks measure their two sides in one interleaved batch rather than
against a recorded number, for the same reason as the shell suites: throughput
on cloud containers drifts more than any regression worth catching.

Opt-in, since it needs a built libpyudepot.so:

    PERF_PYUDEPOT=1 python3 -m pytest bench/test_pyudepot_perf.py -q

Configuration: PERF_ITERATIONS, PERF_OPS.
"""

from __future__ import annotations

import os
import statistics
import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
BENCH = REPO_ROOT / "bench" / "pyudepot_bench.py"
LIB = REPO_ROOT / "python" / "pyudepot" / "libpyudepot.so"

DEFAULT_ITERATIONS = 3
DEFAULT_OPS = 20000

# A per-operation ctypes round trip is expected to cost far more than a
# batched C++ path. This bounds the gap against a collapse; it is not a
# statement about acceptable overhead.
MAX_PUT_LATENCY_US = 5000.0

_skip = None
if os.environ.get("PERF_PYUDEPOT", "").strip().lower() not in {"1", "true", "yes", "on"}:
    _skip = "pyudepot perf tests are opt-in (set PERF_PYUDEPOT=1)"
elif not LIB.exists():
    _skip = "libpyudepot.so not built (make python/pyudepot/libpyudepot.so)"

pytestmark = pytest.mark.skipif(_skip is not None, reason=_skip or "")


def _iterations() -> int:
    return int(os.environ.get("PERF_ITERATIONS", DEFAULT_ITERATIONS))


def _ops() -> int:
    return int(os.environ.get("PERF_OPS", DEFAULT_OPS))


def _run(*args: str) -> dict[str, dict[str, float]]:
    """Run the pyudepot benchmark and return per-phase metrics."""
    result = subprocess.run(
        [sys.executable, str(BENCH), "-n", str(_ops()), *args],
        cwd=REPO_ROOT, capture_output=True, text=True, timeout=1800,
    )
    if result.returncode != 0:
        pytest.skip(f"pyudepot benchmark failed: {result.stderr[-400:]}")

    out: dict[str, dict[str, float]] = {}
    for line in (result.stdout + result.stderr).splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[1].startswith("time="):
            fields = dict(
                p.split("=", 1) for p in parts[1:] if "=" in p
            )
            out[parts[0]] = {
                "secs": float(fields["time"].rstrip("s")),
                "kops": float(fields["Kops/sec"]),
                "mibs": float(fields["MiB/sec"]),
            }
    return out


@pytest.mark.parametrize("phase", ["PUT", "GET"])
def test_throughput_is_sane(phase: str) -> None:
    """The bindings must move data at a plausible rate.

    Guards against a binding change that silently turns every operation into
    something pathological -- a per-byte ctypes conversion, say.
    """
    result = _run()
    assert phase in result, f"benchmark produced no {phase} result"

    kops = result[phase]["kops"]
    assert kops > 0, f"{phase} throughput is zero"

    latency_us = 1000.0 / kops
    assert latency_us <= MAX_PUT_LATENCY_US, (
        f"{phase} latency {latency_us:.1f}us exceeds the "
        f"{MAX_PUT_LATENCY_US:.0f}us bound ({kops:.2f} Kops/sec)"
    )


def test_get_not_slower_than_put() -> None:
    """Reads must not cost more than writes through the bindings.

    uDepot is log-structured, so reads should be at least as cheap as writes.
    A read path that inverts that has usually picked up an extra copy or
    conversion on the way back into numpy. Measured as paired runs so batch
    drift cancels.
    """
    pairs: list[tuple[float, float]] = []
    for _ in range(_iterations()):
        result = _run()
        if "PUT" in result and "GET" in result:
            pairs.append((result["PUT"]["kops"], result["GET"]["kops"]))

    if len(pairs) < 2:
        pytest.skip("insufficient paired samples")

    deltas = [(g - p) / p for p, g in pairs if p > 0]
    median = statistics.median(deltas)

    assert median >= 0, (
        f"GET is {-median:.1%} slower than PUT through the bindings, "
        f"which suggests an extra copy on the read path\n"
        f"  pairs (Kops/sec put->get): "
        + ", ".join(f"{p:.2f}->{g:.2f}" for p, g in pairs)
    )
