"""Performance regression tests for uDepot's KV layer.

Scope: uDepot itself -- its KV interfaces and the Python bindings on top of
them. Raw I/O backends (kernel AIO, io_uring, SPDK) are measured one layer
down in trt/bench/test_trt_io_perf.py; tensor-level behaviour is measured one
layer up in flywheel.

Three things are checked:

    test_no_regression      io_layer_bench and pyudepot_bench, current
                            revision against the change's base revision
    test_zero_copy_faster   the Mbuff (zero-copy) KV interface must not be
                            slower than the raw-buffer one -- uDepot's first
                            design principle, expressed as a test rather than
                            as a recorded number
    test_python_binding_overhead_bounded
                            the ctypes bindings must not lose more than a set
                            multiple of the C++ path's throughput

Each comparison measures both sides in one interleaved batch; see perflib.

Opt-in, because these need a built uDepot and are slow:

    PERF_AB=1        python -m pytest bench/test_udepot_perf.py -q
    PERF_ZEROCOPY=1  python -m pytest bench/test_udepot_perf.py -q

Configuration: PERF_ITERATIONS, PERF_THRESHOLD, PERF_BASE_REF, UDEPOT_ROOT.
"""

from __future__ import annotations

import statistics
import sys
import tempfile
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_REPO_ROOT / "trt" / "bench"))
import perflib  # noqa: E402

# io_layer_bench needs >=200k ops for the comparison to mean anything; below
# that the PUT phase never becomes I/O bound and the result inverts at random.
IO_LAYER_OPS = ["-n", "200000"]
PYUDEPOT_OPS = ["-n", "20000"]

BUILD_TARGETS = ["bench/io_layer_bench", "python/pyudepot/libpyudepot.so"]

# Every entry emits PUT and GET lines.
BENCHMARKS = [
    {
        "key": "io_layer_mbuff_aio",
        "kind": "native",
        "binary": "bench/io_layer_bench",
        "args": ["--mbuff", "--aio"] + IO_LAYER_OPS,
        "description": "KV Mbuff (zero-copy) interface, TRT AIO",
    },
    {
        "key": "io_layer_copy_aio",
        "kind": "native",
        "binary": "bench/io_layer_bench",
        "args": ["--copy", "--aio"] + IO_LAYER_OPS,
        "description": "KV raw-buffer interface, TRT AIO",
    },
    {
        "key": "io_layer_mbuff_uring",
        "kind": "native",
        "binary": "bench/io_layer_bench",
        "args": ["--mbuff", "--uring"] + IO_LAYER_OPS,
        "description": "KV Mbuff (zero-copy) interface, TRT io_uring",
    },
    {
        "key": "io_layer_copy_uring",
        "kind": "native",
        "binary": "bench/io_layer_bench",
        "args": ["--copy", "--uring"] + IO_LAYER_OPS,
        "description": "KV raw-buffer interface, TRT io_uring",
    },
    {
        "key": "pyudepot_odirect",
        "kind": "python",
        "binary": "bench/pyudepot_bench.py",
        "args": ["--backend", "3"] + PYUDEPOT_OPS,
        "description": "pyudepot ctypes bindings, O_DIRECT",
    },
]

BENCH_BY_KEY = {b["key"]: b for b in BENCHMARKS}
PHASES = ["PUT", "GET"]

# The bindings are a per-operation ctypes round trip against a C++ path that
# batches across TRT tasks, so they are expected to be substantially slower.
# This only guards against a collapse, not against ordinary overhead.
MAX_BINDING_SLOWDOWN = 25.0

_udepot_root = perflib.find_udepot_root(Path(__file__).resolve().parent)

_ab_skip = None
if not perflib.env_flag("PERF_AB"):
    _ab_skip = "A/B perf tests are opt-in (set PERF_AB=1)"
elif _udepot_root is None:
    _ab_skip = "uDepot root not found (set UDEPOT_ROOT)"

_zc_skip = None
if not perflib.env_flag("PERF_ZEROCOPY"):
    _zc_skip = "zero-copy invariant test is opt-in (set PERF_ZEROCOPY=1)"
elif _udepot_root is None:
    _zc_skip = "uDepot root not found (set UDEPOT_ROOT)"


@pytest.fixture(scope="module")
def current_build() -> Path:
    """Build uDepot's benchmarks and the Python bindings."""
    assert _udepot_root is not None
    perflib.build(_udepot_root, BUILD_TARGETS)
    return _udepot_root


@pytest.fixture(scope="module")
def base_build() -> Path:
    """Build the benchmarks at the change's base revision, in a worktree."""
    assert _udepot_root is not None

    base_rev = perflib.resolve_base_ref(_udepot_root)
    if base_rev is None:
        pytest.skip("could not resolve a base revision (set PERF_BASE_REF)")
    if base_rev == perflib.git(["rev-parse", "HEAD"], _udepot_root):
        pytest.skip("HEAD is the base revision -- nothing to compare")

    tmp = Path(tempfile.mkdtemp(prefix="udepot-perf-base-"))
    worktree = tmp / "udepot"
    try:
        yield perflib.prepare_base_worktree(
            _udepot_root, base_rev, worktree, BUILD_TARGETS
        )
    finally:
        perflib.remove_worktree(_udepot_root, worktree)


def _variant(root: Path, spec: dict) -> perflib.Variant:
    """Build the (binary, cwd, args) triple for a spec in a checkout."""
    target = root / spec["binary"]
    if spec["kind"] == "python":
        return (Path(sys.executable), root, [str(target)] + spec["args"])
    return (target, root, spec["args"])


def _exists(root: Path, spec: dict) -> bool:
    return (root / spec["binary"]).exists()


_AB_CASES = [
    (b["key"], phase, metric)
    for b in BENCHMARKS
    for phase in PHASES
    for metric in perflib.METRICS
]


@pytest.mark.skipif(_ab_skip is not None, reason=_ab_skip or "")
@pytest.mark.parametrize(
    "case", _AB_CASES, ids=lambda c: f"{c[0]}-{c[1]}-{c[2]}"
)
def test_no_regression(base_build: Path, current_build: Path, case: tuple) -> None:
    """Current revision must not be slower than the base beyond the threshold."""
    key, phase, metric = case
    spec = BENCH_BY_KEY[key]

    if not _exists(base_build, spec):
        pytest.skip(f"{spec['binary']} not built at base revision")
    if not _exists(current_build, spec):
        pytest.skip(f"{spec['binary']} not built")

    pairs = perflib.cached_pairs(
        (f"base:{key}", _variant(base_build, spec)),
        (f"cur:{key}", _variant(current_build, spec)),
        PHASES,
        perflib.iterations(),
    )[phase]

    if len(pairs) < 2:
        pytest.skip(f"{key}/{phase}: insufficient paired samples")

    regression = -statistics.median(perflib.relative_deltas(pairs, metric))
    limit = perflib.threshold()

    assert regression <= limit, (
        f"{key} ({spec['description']}) {phase} {metric}: median regression "
        f"{regression:.1%} exceeds {limit:.0%}\n"
        f"  A = base revision, B = current revision\n"
        f"{perflib.summarize(pairs, metric)}"
    )


_ZC_CASES = [
    (backend, phase, metric)
    for backend in ["aio", "uring"]
    for phase in PHASES
    for metric in perflib.METRICS
]


@pytest.mark.skipif(_zc_skip is not None, reason=_zc_skip or "")
@pytest.mark.parametrize("case", _ZC_CASES, ids=lambda c: f"{c[0]}-{c[1]}-{c[2]}")
def test_zero_copy_faster(current_build: Path, case: tuple) -> None:
    """The Mbuff (zero-copy) KV interface must not be slower than raw buffers.

    Compares the two interfaces within a single build, so it needs no base
    revision. This is uDepot's zero-copy design principle stated as a test.
    """
    backend, phase, metric = case
    copy_spec = BENCH_BY_KEY[f"io_layer_copy_{backend}"]
    mbuff_spec = BENCH_BY_KEY[f"io_layer_mbuff_{backend}"]

    if not _exists(current_build, copy_spec):
        pytest.skip("io_layer_bench not built")

    pairs = perflib.cached_pairs(
        (f"copy:{backend}", _variant(current_build, copy_spec)),
        (f"mbuff:{backend}", _variant(current_build, mbuff_spec)),
        PHASES,
        perflib.iterations(),
    )[phase]

    if len(pairs) < 2:
        pytest.skip(f"zero-copy {backend}/{phase}: insufficient paired samples")

    improvement = statistics.median(perflib.relative_deltas(pairs, metric))

    assert improvement >= 0, (
        f"zero-copy regression: the Mbuff interface is slower than the "
        f"raw-buffer one on {backend} {phase} {metric} "
        f"(median {improvement:+.1%})\n"
        f"  A = raw buffer, B = Mbuff (zero-copy)\n"
        f"{perflib.summarize(pairs, metric)}"
    )


@pytest.mark.skipif(_zc_skip is not None, reason=_zc_skip or "")
@pytest.mark.parametrize("phase", PHASES)
def test_python_binding_overhead_bounded(current_build: Path, phase: str) -> None:
    """The ctypes bindings must stay within a bounded factor of the C++ path.

    The bindings do a per-operation round trip where the C++ benchmark batches
    across TRT tasks, so a large constant gap is expected and fine. This guards
    only against that gap collapsing into something pathological.
    """
    py_spec = BENCH_BY_KEY["pyudepot_odirect"]
    if not _exists(current_build, py_spec):
        pytest.skip("pyudepot_bench.py not present")
    if not (current_build / "python" / "pyudepot" / "libpyudepot.so").exists():
        pytest.skip("libpyudepot.so not built")

    py = perflib.run_once(*_variant(current_build, py_spec))
    if phase not in py:
        pytest.skip("pyudepot benchmark produced no result")

    native_spec = BENCH_BY_KEY["io_layer_copy_aio"]
    if not _exists(current_build, native_spec):
        pytest.skip("io_layer_bench not built")
    native = perflib.run_once(*_variant(current_build, native_spec))
    if phase not in native:
        pytest.skip("io_layer benchmark produced no result")

    py_kops = py[phase]["kops_sec"]
    native_kops = native[phase]["kops_sec"]
    assert py_kops > 0, f"pyudepot {phase} throughput is zero"

    slowdown = native_kops / py_kops
    assert slowdown <= MAX_BINDING_SLOWDOWN, (
        f"pyudepot {phase} is {slowdown:.1f}x slower than the C++ path, "
        f"above the {MAX_BINDING_SLOWDOWN:.0f}x bound\n"
        f"  pyudepot: {py_kops:.2f} Kops/sec\n"
        f"  native:   {native_kops:.2f} Kops/sec"
    )
