#!/usr/bin/env python3
"""Run TRT AIO and io_uring benchmarks, collect statistics, and output JSON.

Usage:
    python3 bench/run_benchmarks.py [--iterations N] [--output FILE]
    python3 bench/run_benchmarks.py --record-baseline [--output FILE]

The script builds the TRT benchmarks (if needed), runs each one N times,
and reports per-backend stats: median, mean, stddev, min, max, p5, p95
for Kops/sec and MiB/sec.

With --record-baseline, writes the results as a baseline JSON file that
the flywheel regression tests compare against.
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TRT_DIR = REPO_ROOT / "trt"

BENCHMARKS = {
    "trt_aio": {
        "binary": "build/benchs/trt_aio_bench",
        "description": "TRT AIO (kernel AIO) 4K random read",
    },
    "trt_uring": {
        "binary": "build/benchs/trt_uring_bench",
        "description": "TRT io_uring 4K random read",
    },
}

RESULT_RE = re.compile(
    r"time=(?P<time>[\d.]+)s\s+"
    r"Kops/sec=(?P<kops>[\d.]+)\s+"
    r"MiB/sec=(?P<mibs>[\d.]+)"
)


def build_trt() -> None:
    """Build TRT benchmarks if binaries are missing."""
    missing = [
        name for name, info in BENCHMARKS.items()
        if not (TRT_DIR / info["binary"]).exists()
    ]
    if not missing:
        return

    print("Building TRT benchmarks...", file=sys.stderr)
    uring_lib = TRT_DIR / "external" / "liburing" / "src" / "liburing.a"
    if not uring_lib.exists():
        subprocess.run(
            ["make", "build_uring"],
            cwd=TRT_DIR, check=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
    subprocess.run(
        ["make", "-j", str(os.cpu_count() or 1)],
        cwd=TRT_DIR, check=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def run_single(binary_path: Path) -> dict[str, float] | None:
    """Run a single benchmark invocation, parse its output."""
    try:
        result = subprocess.run(
            [str(binary_path)],
            cwd=TRT_DIR,
            capture_output=True, text=True, timeout=120,
        )
    except subprocess.TimeoutExpired:
        print(f"  TIMEOUT: {binary_path.name}", file=sys.stderr)
        return None

    if result.returncode != 0:
        print(f"  FAILED (rc={result.returncode}): {binary_path.name}",
              file=sys.stderr)
        return None

    output = result.stdout + result.stderr
    match = RESULT_RE.search(output)
    if not match:
        print(f"  NO MATCH in output: {output[:200]}", file=sys.stderr)
        return None

    return {
        "time_s": float(match.group("time")),
        "kops_sec": float(match.group("kops")),
        "mibs_sec": float(match.group("mibs")),
    }


def compute_stats(samples: list[float]) -> dict[str, float]:
    """Compute summary statistics for a list of samples."""
    s = sorted(samples)
    n = len(s)
    return {
        "n": n,
        "mean": statistics.mean(s),
        "median": statistics.median(s),
        "stddev": statistics.stdev(s) if n > 1 else 0.0,
        "min": s[0],
        "max": s[-1],
        "p5": s[max(0, int(n * 0.05))],
        "p95": s[min(n - 1, int(n * 0.95))],
    }


def run_benchmarks(iterations: int) -> dict:
    """Run all benchmarks and collect results."""
    build_trt()
    results = {}

    for name, info in BENCHMARKS.items():
        binary = TRT_DIR / info["binary"]
        if not binary.exists():
            print(f"SKIP {name}: binary not found", file=sys.stderr)
            continue

        print(f"Running {name} ({iterations} iterations)...", file=sys.stderr)
        kops_samples = []
        mibs_samples = []

        for i in range(iterations):
            data = run_single(binary)
            if data is None:
                continue
            kops_samples.append(data["kops_sec"])
            mibs_samples.append(data["mibs_sec"])
            print(f"  [{i+1}/{iterations}] "
                  f"{data['kops_sec']:.1f} Kops/s  "
                  f"{data['mibs_sec']:.1f} MiB/s", file=sys.stderr)

        if len(kops_samples) < 2:
            print(f"SKIP {name}: insufficient samples", file=sys.stderr)
            continue

        results[name] = {
            "description": info["description"],
            "kops_sec": compute_stats(kops_samples),
            "mibs_sec": compute_stats(mibs_samples),
        }

    return results


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--iterations", "-n", type=int, default=5,
        help="Number of benchmark iterations (default: 5)",
    )
    parser.add_argument(
        "--output", "-o", type=str, default=None,
        help="Write JSON results to this file (default: stdout)",
    )
    parser.add_argument(
        "--record-baseline", action="store_true",
        help="Record results as baseline for regression testing",
    )
    args = parser.parse_args()

    results = run_benchmarks(args.iterations)

    output = {
        "benchmarks": results,
        "config": {
            "iterations": args.iterations,
            "nops_total": 1024 * 1024,
            "buff_size": 4096,
            "file_size_mb": 128,
        },
    }

    json_str = json.dumps(output, indent=2)

    if args.output:
        Path(args.output).write_text(json_str + "\n")
        print(f"Results written to {args.output}", file=sys.stderr)
    else:
        print(json_str)


if __name__ == "__main__":
    main()
