"""Performance regression tests for TRT's raw I/O backends.

Scope: the raw I/O layer only -- kernel AIO, io_uring, and SPDK bdev. These
benchmarks issue reads directly through TRT with no KV store above them, so
they measure the I/O path in isolation. Anything involving uDepot's KV
interfaces belongs in uDepot's suite, not here.

Each run measures both revisions itself rather than comparing against a
recorded number; see perflib for why.

Opt-in, because it needs a built TRT and is slow:

    PERF_AB=1 python -m pytest trt/bench/test_trt_io_perf.py -q

Configuration: PERF_ITERATIONS, PERF_THRESHOLD, PERF_BASE_REF, UDEPOT_ROOT.
"""

from __future__ import annotations

import statistics
import sys
import tempfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import perflib  # noqa: E402

TRT_BENCH_DIR = "trt/build/benchs"

# Raw I/O benchmarks. Each emits a single unlabelled result line.
BENCHMARKS = [
    {
        "key": "trt_aio",
        "binary": "trt_aio_bench",
        "description": "TRT kernel-AIO 4K random read",
        "requires": None,
    },
    {
        "key": "trt_uring",
        "binary": "trt_uring_bench",
        "description": "TRT io_uring 4K random read",
        "requires": None,
    },
    {
        "key": "spdk_bdev_malloc",
        "binary": "spdk_bdev_bench",
        "description": "SPDK bdev_malloc 4K random read",
        "requires": "BUILD_SPDK=1",
    },
]

BENCH_BY_KEY = {b["key"]: b for b in BENCHMARKS}

_udepot_root = perflib.find_udepot_root(Path(__file__).resolve().parent)

_skip = None
if not perflib.env_flag("PERF_AB"):
    _skip = "raw I/O A/B perf tests are opt-in (set PERF_AB=1)"
elif _udepot_root is None:
    _skip = "uDepot root not found (set UDEPOT_ROOT)"


@pytest.fixture(scope="module")
def current_build() -> Path:
    """Build TRT in the working checkout."""
    assert _udepot_root is not None
    perflib.build(_udepot_root)
    return _udepot_root


@pytest.fixture(scope="module")
def base_build() -> Path:
    """Build TRT at the change's base revision, in a worktree."""
    assert _udepot_root is not None

    base_rev = perflib.resolve_base_ref(_udepot_root)
    if base_rev is None:
        pytest.skip("could not resolve a base revision (set PERF_BASE_REF)")
    if base_rev == perflib.git(["rev-parse", "HEAD"], _udepot_root):
        pytest.skip("HEAD is the base revision -- nothing to compare")

    tmp = Path(tempfile.mkdtemp(prefix="trt-perf-base-"))
    worktree = tmp / "udepot"
    try:
        yield perflib.prepare_base_worktree(_udepot_root, base_rev, worktree)
    finally:
        perflib.remove_worktree(_udepot_root, worktree)


def _variant(root: Path, spec: dict) -> perflib.Variant:
    return (root / TRT_BENCH_DIR / spec["binary"], root / "trt", [])


_CASES = [(b["key"], m) for b in BENCHMARKS for m in perflib.METRICS]


@pytest.mark.skipif(_skip is not None, reason=_skip or "")
@pytest.mark.parametrize("case", _CASES, ids=lambda c: f"{c[0]}-{c[1]}")
def test_no_regression(base_build: Path, current_build: Path, case: tuple) -> None:
    """Current revision must not be slower than the base beyond the threshold."""
    key, metric = case
    spec = BENCH_BY_KEY[key]

    if not (base_build / TRT_BENCH_DIR / spec["binary"]).exists():
        pytest.skip(f"{spec['binary']} not built at base revision")
    if not (current_build / TRT_BENCH_DIR / spec["binary"]).exists():
        requires = spec.get("requires")
        pytest.skip(
            f"{spec['binary']} not built" + (f" (requires {requires})" if requires else "")
        )

    pairs = perflib.cached_pairs(
        (f"base:{key}", _variant(base_build, spec)),
        (f"cur:{key}", _variant(current_build, spec)),
        [None],
        perflib.iterations(),
    )[None]

    if len(pairs) < 2:
        pytest.skip(f"{key}: insufficient paired samples")

    regression = -statistics.median(perflib.relative_deltas(pairs, metric))
    limit = perflib.threshold()

    assert regression <= limit, (
        f"{key} ({spec['description']}) {metric}: median regression "
        f"{regression:.1%} exceeds {limit:.0%}\n"
        f"  A = base revision, B = current revision\n"
        f"{perflib.summarize(pairs, metric)}"
    )
