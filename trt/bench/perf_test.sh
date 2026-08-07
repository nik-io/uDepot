#!/bin/bash
#
#  Copyright (c) 2020 International Business Machines
#  All rights reserved.
#
#  SPDX-License-Identifier: BSD-3-Clause
#
#  Performance regression test for TRT's raw I/O backends.
#
#  Scope: the raw I/O layer only -- kernel AIO, io_uring, SPDK bdev. These
#  benchmarks issue reads directly through TRT with no KV store above them.
#  Anything touching uDepot's KV interfaces is measured one layer up, in
#  uDepot's bench/perf_test.sh.
#
#  Builds the benchmarks at the change's base revision in a worktree and runs
#  them alternately against the current build, so shared drift cancels out.
#
#  Usage:
#      trt/bench/perf_test.sh [benchmark ...]
#
#  Environment:
#      PERF_ITERATIONS  paired iterations (default 5)
#      PERF_THRESHOLD   max tolerated median regression (default 0.10)
#      PERF_BASE_REF    base revision (default: merge-base with origin/main)
#      UDEPOT_ROOT      uDepot checkout
#

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=trt/bench/perflib.sh
. "$HERE/perflib.sh"

BENCH_DIR="trt/build/benchs"

# name:binary:description
BENCHMARKS=(
	"trt_aio:trt_aio_bench:TRT kernel-AIO 4K random read"
	"trt_uring:trt_uring_bench:TRT io_uring 4K random read"
	"spdk_bdev_malloc:spdk_bdev_bench:SPDK bdev_malloc 4K random read (needs BUILD_SPDK=1)"
)

ROOT="$(pl_udepot_root)"
WANTED=("$@")
rc=0

want() {
	[ ${#WANTED[@]} -eq 0 ] && return 0
	local w
	for w in "${WANTED[@]}"; do [ "$w" = "$1" ] && return 0; done
	return 1
}

BASE_REV="$(pl_base_rev "$ROOT")"
[ -n "$BASE_REV" ] || pl_die "could not resolve a base revision (set PERF_BASE_REF)"
HEAD_REV="$(git -C "$ROOT" rev-parse HEAD)"
if [ "$BASE_REV" = "$HEAD_REV" ]; then
	echo "HEAD is the base revision -- nothing to compare"
	exit 0
fi

echo "base:    $(git -C "$ROOT" rev-parse --short "$BASE_REV")"
echo "current: $(git -C "$ROOT" rev-parse --short "$HEAD_REV")"
echo "iterations: $PERF_ITERATIONS  threshold: $PERF_THRESHOLD"
echo

TMPDIR_BASE="$(mktemp -d -t trt-perf-base-XXXXXX)"
BASE_WT="$TMPDIR_BASE/udepot"
cleanup() { pl_remove_base "$ROOT" "$BASE_WT"; rm -rf "$TMPDIR_BASE"; }
trap cleanup EXIT

pl_info "building current revision ..."
pl_build "$ROOT"
pl_info "building base revision ..."
pl_prepare_base "$ROOT" "$BASE_REV" "$BASE_WT"

for entry in "${BENCHMARKS[@]}"; do
	IFS=: read -r name binary desc <<<"$entry"
	want "$name" || continue

	base_bin="$BASE_WT/$BENCH_DIR/$binary"
	cur_bin="$ROOT/$BENCH_DIR/$binary"
	if [ ! -x "$base_bin" ] || [ ! -x "$cur_bin" ]; then
		echo "SKIP $name: $binary not built"
		continue
	fi

	# Probe first: a benchmark that cannot run should say why, not drain away
	# into "insufficient samples" and read as noise.
	if ! pl_probe "cd '$ROOT/trt' && '$cur_bin'"; then
		echo "SKIP $name: $binary $PL_PROBE_MSG"
		printf '%s\n' "$PL_PROBE_TAIL" | sed 's/^/    /'
		echo
		continue
	fi

	for metric in kops mibs; do
		echo "TEST $name ($desc) [$metric]"
		out="$(pl_paired "$PERF_ITERATIONS" "-" "$metric" \
			"cd '$BASE_WT/trt' && '$base_bin'" \
			"cd '$ROOT/trt' && '$cur_bin'")"
		printf '%s\n' "$out" | pl_report base current "$PERF_THRESHOLD" regression
		case $? in
			0) ;;
			77) echo "  SKIP: insufficient samples" ;;
			*) rc=1 ;;
		esac
		echo
	done
done

exit $rc
