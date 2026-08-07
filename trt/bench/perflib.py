"""Shared machinery for paired performance comparisons.

This lives in trt because trt is the lowest layer in the stack: uDepot builds
on trt, and flywheel builds on uDepot, so both can import from here without
inverting the dependency direction.

The approach throughout is paired measurement. Nothing is compared against a
recorded baseline, because throughput on cloud containers drifts far more than
any regression worth catching -- in measurements taken while writing this, one
benchmark went from 30.4s to 19.5s across five consecutive iterations on an
idle machine. Instead both sides of a comparison are run alternately in a
single batch, so shared drift cancels out of each per-pair delta.

Two comparison shapes are supported:

    A/B across revisions   run the same benchmark built at two revisions
                           (see `base_worktree`)
    A/B across variants    run two different benchmarks/modes from one build
                           (e.g. zero-copy vs raw-buffer)

Both reduce to `paired_samples`.
"""

from __future__ import annotations

import os
import re
import shutil
import statistics
import subprocess
from pathlib import Path

# Matches both output shapes emitted across the benchmark suite:
#   "time=1.2s Kops/sec=3.4 MiB/sec=5.6"       -- single unlabelled result
#   "PUT time=1.2s Kops/sec=3.4 MiB/sec=5.6"   -- one line per phase
RESULT_RE = re.compile(
    r"^(?:(?P<phase>[A-Z]+)\s+)?time=(?P<time>[\d.]+)s\s+"
    r"Kops/sec=(?P<kops>[\d.]+)\s+MiB/sec=(?P<mibs>[\d.]+)",
    re.MULTILINE,
)

METRICS = ["kops_sec", "mibs_sec"]

DEFAULT_ITERATIONS = 5
DEFAULT_THRESHOLD = 0.10


def env_flag(name: str) -> bool:
    """Return True if the named environment variable is set to a truthy value."""
    return os.environ.get(name, "").strip().lower() in {"1", "true", "yes", "on"}


def iterations() -> int:
    """Number of paired iterations per comparison."""
    return int(os.environ.get("PERF_ITERATIONS", DEFAULT_ITERATIONS))


def threshold() -> float:
    """Maximum tolerated median regression, as a fraction."""
    return float(os.environ.get("PERF_THRESHOLD", DEFAULT_THRESHOLD))


def parse_results(output: str) -> dict[str | None, dict[str, float]]:
    """Parse benchmark stdout into per-phase metrics.

    Args:
        output: Combined stdout/stderr of a benchmark invocation.

    Returns:
        Mapping of phase label (None for unlabelled output) to a dict with
        `kops_sec` and `mibs_sec`. Empty if nothing parsed.
    """
    results: dict[str | None, dict[str, float]] = {}
    for match in RESULT_RE.finditer(output):
        results[match.group("phase")] = {
            "kops_sec": float(match.group("kops")),
            "mibs_sec": float(match.group("mibs")),
        }
    return results


def run_once(
    binary: Path, cwd: Path, args: list[str] | None = None, timeout: int = 600
) -> dict[str | None, dict[str, float]]:
    """Run one benchmark invocation and parse its output.

    Returns an empty dict when the run fails, times out, or emits nothing
    parseable, so callers can drop the sample rather than fail the batch.
    """
    try:
        result = subprocess.run(
            [str(binary)] + (args or []),
            cwd=cwd,
            capture_output=True, text=True, timeout=timeout,
        )
    except (subprocess.TimeoutExpired, OSError):
        return {}
    if result.returncode != 0:
        return {}
    return parse_results(result.stdout + result.stderr)


Variant = tuple[Path, Path, list[str]]  # (binary, cwd, args)
Pair = tuple[dict[str, float], dict[str, float]]


def paired_samples(
    a: Variant, b: Variant, phases: list[str | None], count: int
) -> dict[str | None, list[Pair]]:
    """Run two variants alternately and collect paired samples.

    Alternating within one batch is what makes the comparison valid: throughput
    drifts steadily over a batch, and pairing cancels that drift out of each
    per-pair delta.

    Args:
        a: First variant.
        b: Second variant.
        phases: Phase labels to collect.
        count: Number of paired iterations.

    Returns:
        Mapping of phase to a list of (a_metrics, b_metrics) pairs. Pairs where
        either side produced no result are dropped.
    """
    pairs: dict[str | None, list[Pair]] = {phase: [] for phase in phases}
    for _ in range(count):
        res_a = run_once(*a)
        res_b = run_once(*b)
        for phase in phases:
            if phase in res_a and phase in res_b:
                pairs[phase].append((res_a[phase], res_b[phase]))
    return pairs


def relative_deltas(pairs: list[Pair], metric: str) -> list[float]:
    """Per-pair relative change from the first variant to the second.

    Positive means the second variant is faster.
    """
    return [
        (b[metric] - a[metric]) / a[metric]
        for a, b in pairs
        if a[metric] > 0
    ]


def summarize(pairs: list[Pair], metric: str) -> str:
    """Render a human-readable summary of a paired comparison."""
    a_vals = [a[metric] for a, _ in pairs]
    b_vals = [b[metric] for _, b in pairs]
    deltas = relative_deltas(pairs, metric)
    wins = sum(1 for d in deltas if d > 0)
    return "\n".join([
        f"  n={len(pairs)}  wins={wins}/{len(deltas)}",
        f"  A: median={statistics.median(a_vals):.2f}  "
        f"min={min(a_vals):.2f}  max={max(a_vals):.2f}",
        f"  B: median={statistics.median(b_vals):.2f}  "
        f"min={min(b_vals):.2f}  max={max(b_vals):.2f}",
        f"  per-pair delta: median={statistics.median(deltas):+.1%}  "
        f"mean={statistics.mean(deltas):+.1%}",
        "  pairs: " + ", ".join(f"{a:.1f}->{b:.1f}" for a, b in zip(a_vals, b_vals)),
    ])


# ---------------------------------------------------------------------------
# Building and revision handling
# ---------------------------------------------------------------------------

def git(args: list[str], cwd: Path, check: bool = True) -> str:
    """Run a git command and return its stripped stdout."""
    return subprocess.run(
        ["git"] + args, cwd=cwd, check=check,
        capture_output=True, text=True, timeout=120,
    ).stdout.strip()


def build(udepot_root: Path, targets: list[str] | None = None) -> None:
    """Build TRT and (optionally) specific uDepot targets in a checkout.

    Args:
        udepot_root: Root of the uDepot checkout to build in.
        targets: Extra make targets to build from the uDepot root.
    """
    trt_dir = udepot_root / "trt"

    uring_lib = trt_dir / "external" / "liburing" / "src" / "liburing.a"
    if not uring_lib.exists() and (trt_dir / "external" / "liburing" / "Makefile").exists():
        subprocess.run(
            ["make", "build_uring"],
            cwd=trt_dir, check=True, capture_output=True, timeout=900,
        )

    make_args = ["-j", str(os.cpu_count() or 1), "BUILD_URING=1"]
    subprocess.run(
        ["make"] + make_args,
        cwd=trt_dir, check=True, capture_output=True, timeout=1800,
    )
    for target in targets or []:
        subprocess.run(
            ["make"] + make_args + [target],
            cwd=udepot_root, check=True, capture_output=True, timeout=1800,
        )


def resolve_base_ref(udepot_root: Path) -> str | None:
    """Resolve the revision to treat as 'before' for an A/B comparison."""
    explicit = os.environ.get("PERF_BASE_REF")
    if explicit:
        try:
            return git(["rev-parse", explicit], udepot_root)
        except subprocess.CalledProcessError:
            return None

    for base in ["origin/main", "origin/master", "main", "master"]:
        try:
            return git(["merge-base", "HEAD", base], udepot_root)
        except subprocess.CalledProcessError:
            continue
    return None


def prepare_base_worktree(
    udepot_root: Path, base_rev: str, dest: Path, targets: list[str] | None = None
) -> Path:
    """Check out `base_rev` into a worktree and build its benchmarks.

    Vendored dependencies (liburing, cityhash) are copied from the primary
    checkout rather than re-fetched. Keeping them byte-identical on both sides
    isolates the change under test, which is the point of the comparison.

    Args:
        udepot_root: The primary uDepot checkout.
        base_rev: Revision to check out.
        dest: Directory to create the worktree in.
        targets: Extra make targets to build.

    Returns:
        Path to the prepared worktree.
    """
    git(["worktree", "add", "--detach", str(dest), base_rev], udepot_root)

    for rel in [Path("trt/external/liburing"), Path("external/cityhash")]:
        src = udepot_root / rel
        dst = dest / rel
        if src.is_dir() and not (dst / ".git").exists() and not any(dst.glob("*")):
            if dst.exists():
                shutil.rmtree(dst)
            shutil.copytree(src, dst, symlinks=True)

    build(dest, targets)
    return dest


def remove_worktree(udepot_root: Path, worktree: Path) -> None:
    """Remove a worktree created by `prepare_base_worktree`."""
    subprocess.run(
        ["git", "worktree", "remove", "--force", str(worktree)],
        cwd=udepot_root, capture_output=True,
    )


def find_udepot_root(start: Path) -> Path | None:
    """Locate the uDepot checkout containing or near `start`."""
    env = os.environ.get("UDEPOT_ROOT")
    if env and Path(env).is_dir():
        return Path(env)

    for candidate in [start, *start.parents]:
        if (candidate / "trt").is_dir() and (candidate / "src" / "uDepot").is_dir():
            return candidate
    return None


# ---------------------------------------------------------------------------
# Batch caching
# ---------------------------------------------------------------------------

# One batch serves every phase and metric of a comparison. Without this each
# parametrized case would re-run the whole batch, repeating slow invocations
# once per phase and metric for identical data.
_pair_cache: dict[tuple, dict] = {}


def cached_pairs(
    a: tuple[str, Variant], b: tuple[str, Variant], phases: list[str | None], count: int
) -> dict[str | None, list[Pair]]:
    """Run a paired batch once and reuse it across phases and metrics.

    Args:
        a: (cache key, variant) for the first side.
        b: (cache key, variant) for the second side.
        phases: Phase labels to collect.
        count: Number of paired iterations.

    Returns:
        Mapping of phase to the list of paired samples.
    """
    a_key, a_variant = a
    b_key, b_variant = b
    key = (a_key, b_key, tuple(str(p) for p in phases), count)
    if key not in _pair_cache:
        _pair_cache[key] = paired_samples(a_variant, b_variant, phases, count)
    return _pair_cache[key]
