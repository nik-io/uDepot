#!/bin/bash
#
#  Copyright (c) 2020 International Business Machines
#  All rights reserved.
#
#  SPDX-License-Identifier: BSD-3-Clause
#
#  Shared machinery for paired performance comparisons.
#
#  This lives in trt because trt is the lowest layer in the stack: uDepot
#  builds on trt, so both can source it without inverting the dependency
#  direction.
#
#  Nothing is compared against a recorded baseline. Throughput on cloud
#  containers drifts far more than any regression worth catching -- one
#  benchmark here went from 30.4s to 19.5s across five consecutive iterations
#  on an idle machine. Instead both sides of a comparison run alternately in a
#  single batch, so shared drift cancels out of each per-pair delta.
#
#  Source this file, then use pl_paired + pl_report.
#

PERF_ITERATIONS="${PERF_ITERATIONS:-5}"
PERF_THRESHOLD="${PERF_THRESHOLD:-0.10}"

pl_die() { echo "perflib: $*" >&2; exit 1; }
pl_info() { echo "$*" >&2; }

# Extract "<phase> <kops> <mibs>" triples from benchmark output.
# Handles both shapes emitted across the suite:
#   "time=1.2s Kops/sec=3.4 MiB/sec=5.6"       -> phase "-"
#   "PUT time=1.2s Kops/sec=3.4 MiB/sec=5.6"   -> phase "PUT"
pl_extract() {
	awk '
	/time=[0-9.]+s[ \t]+Kops\/sec=/ {
		phase = ($1 ~ /^time=/) ? "-" : $1
		kops = ""; mibs = ""
		for (i = 1; i <= NF; i++) {
			if ($i ~ /^Kops\/sec=/) { split($i, a, "="); kops = a[2] }
			if ($i ~ /^MiB\/sec=/)  { split($i, a, "="); mibs = a[2] }
		}
		if (kops != "") print phase, kops, mibs
	}'
}

# pl_value <phase> <metric:kops|mibs>
# Reads benchmark output on stdin, prints the single requested value.
pl_value() {
	local phase="$1" metric="$2"
	pl_extract | awk -v p="$phase" -v m="$metric" '
		$1 == p { print (m == "kops") ? $2 : $3; exit }'
}

# Per-invocation wall-clock limit, so a hung benchmark cannot wedge the run.
PERF_RUN_TIMEOUT="${PERF_RUN_TIMEOUT:-900}"

# pl_probe <cmd> -- run once; non-zero if it fails or emits no result.
# Use this before a batch so a broken benchmark reports as broken rather than
# silently draining away into "insufficient samples".
pl_probe() {
	local out rc
	out="$(timeout "$PERF_RUN_TIMEOUT" bash -c "$1" 2>&1)"
	rc=$?
	if [ $rc -ne 0 ]; then
		PL_PROBE_MSG="exited rc=$rc"
		PL_PROBE_TAIL="$(printf '%s\n' "$out" | tail -3)"
		return 1
	fi
	if [ -z "$(printf '%s\n' "$out" | pl_extract)" ]; then
		PL_PROBE_MSG="produced no parseable result"
		PL_PROBE_TAIL="$(printf '%s\n' "$out" | tail -3)"
		return 1
	fi
	return 0
}

# pl_paired <iterations> <phase> <metric> <cmd_a> <cmd_b>
# Runs the two commands alternately and prints one "a b" line per iteration.
# Iterations where either side yields nothing are dropped.
pl_paired() {
	local n="$1" phase="$2" metric="$3" cmd_a="$4" cmd_b="$5"
	local i out_a out_b va vb

	for ((i = 0; i < n; i++)); do
		out_a="$(timeout "$PERF_RUN_TIMEOUT" bash -c "$cmd_a" 2>&1)"
		out_b="$(timeout "$PERF_RUN_TIMEOUT" bash -c "$cmd_b" 2>&1)"
		va="$(printf '%s\n' "$out_a" | pl_value "$phase" "$metric")"
		vb="$(printf '%s\n' "$out_b" | pl_value "$phase" "$metric")"
		if [ -n "$va" ] && [ -n "$vb" ]; then
			echo "$va $vb"
		fi
		pl_info "  [$((i + 1))/$n] ${va:-NA} -> ${vb:-NA}"
	done
}

# pl_report <label_a> <label_b> <threshold> <mode:regression|improvement>
# Reads "a b" pairs on stdin. Prints a summary and returns non-zero on failure.
#
#   regression  : fail when B is more than <threshold> below A
#   improvement : fail when B is below A at all (paired invariant)
pl_report() {
	local label_a="$1" label_b="$2" thresh="$3" mode="$4"
	awk -v la="$label_a" -v lb="$label_b" -v th="$thresh" -v mode="$mode" '
	# Insertion sort, not asort(): asort is a gawk extension and this has to
	# run under mawk too.
	function median(arr, n,   i, j, t, c) {
		for (i = 1; i <= n; i++) c[i] = arr[i]
		for (i = 2; i <= n; i++) {
			t = c[i]
			for (j = i - 1; j >= 1 && c[j] > t; j--) c[j + 1] = c[j]
			c[j + 1] = t
		}
		return (n % 2) ? c[(n + 1) / 2] : (c[n / 2] + c[n / 2 + 1]) / 2.0
	}
	{
		a[++n] = $1; b[n] = $2
		d[n] = ($1 > 0) ? ($2 - $1) / $1 : 0
		if (d[n] > 0) wins++
		pairs = pairs sprintf("%s%.1f->%.1f", (pairs == "" ? "" : ", "), $1, $2)
	}
	END {
		if (n < 2) {
			printf "  insufficient paired samples (n=%d)\n", n
			exit 77
		}
		ma = median(a, n); mb = median(b, n); md = median(d, n)
		printf "  n=%d  %s faster in %d/%d pairs\n", n, lb, wins, n
		printf "  %s: median=%.2f\n", la, ma
		printf "  %s: median=%.2f\n", lb, mb
		printf "  per-pair delta: median=%+.1f%%\n", md * 100
		printf "  pairs: %s\n", pairs
		if (mode == "improvement") {
			if (md < 0) {
				printf "  FAIL: %s is slower than %s\n", lb, la
				exit 1
			}
		} else {
			if (-md > th) {
				printf "  FAIL: regression %.1f%% exceeds %.0f%%\n", -md * 100, th * 100
				exit 1
			}
		}
		printf "  OK\n"
	}'
}

# ---------------------------------------------------------------------------
# Building and revision handling
# ---------------------------------------------------------------------------

pl_udepot_root() {
	local dir="${UDEPOT_ROOT:-}"
	if [ -n "$dir" ] && [ -d "$dir" ]; then echo "$dir"; return 0; fi
	dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
	[ -d "$dir/trt" ] || pl_die "cannot locate uDepot root (set UDEPOT_ROOT)"
	echo "$dir"
}

# pl_build <root> [make targets...]
pl_build() {
	local root="$1"; shift
	local jobs; jobs="$(nproc 2>/dev/null || echo 1)"

	if [ ! -f "$root/trt/external/liburing/src/liburing.a" ] &&
	   [ -f "$root/trt/external/liburing/Makefile" ]; then
		make -C "$root/trt" build_uring >/dev/null 2>&1 || true
	fi
	make -C "$root/trt" -j "$jobs" BUILD_URING=1 >/dev/null ||
		pl_die "trt build failed in $root"
	local t
	for t in "$@"; do
		make -C "$root" -j "$jobs" BUILD_URING=1 "$t" >/dev/null ||
			pl_die "build of $t failed in $root"
	done
}

# pl_base_rev <root> -- revision to treat as "before"
pl_base_rev() {
	local root="$1" rev base
	if [ -n "${PERF_BASE_REF:-}" ]; then
		git -C "$root" rev-parse "$PERF_BASE_REF" 2>/dev/null
		return
	fi
	for base in origin/main origin/master main master; do
		rev="$(git -C "$root" merge-base HEAD "$base" 2>/dev/null)" || continue
		[ -n "$rev" ] && { echo "$rev"; return; }
	done
}

# pl_prepare_base <root> <rev> <dest> [make targets...]
#
# Vendored dependencies are copied from the primary checkout rather than
# re-fetched: keeping them byte-identical on both sides isolates the change
# under test, which is the point of the comparison.
pl_prepare_base() {
	local root="$1" rev="$2" dest="$3"; shift 3
	git -C "$root" worktree add --detach "$dest" "$rev" >/dev/null 2>&1 ||
		pl_die "could not create worktree at $dest"

	local rel
	for rel in trt/external/liburing external/cityhash; do
		if [ -d "$root/$rel" ] && [ -z "$(ls -A "$dest/$rel" 2>/dev/null)" ]; then
			mkdir -p "$(dirname "$dest/$rel")"
			cp -a "$root/$rel" "$dest/$rel"
		fi
	done

	pl_build "$dest" "$@"
}

pl_remove_base() {
	local root="$1" dest="$2"
	git -C "$root" worktree remove --force "$dest" >/dev/null 2>&1 || true
}
