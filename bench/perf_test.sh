#!/bin/bash
#
#  Copyright (c) 2020 International Business Machines
#  All rights reserved.
#
#  SPDX-License-Identifier: BSD-3-Clause
#
#  Performance regression test for uDepot's KV layer.
#
#  Scope: uDepot's own KV interfaces. Raw I/O backends are measured one layer
#  down in trt/bench/perf_test.sh; the Python bindings have their own test in
#  bench/test_pyudepot_perf.py because they are Python; tensor-level behaviour
#  is measured one layer up in flywheel.
#
#  Two checks:
#
#    zerocopy   The Mbuff (zero-copy) KV interface must not be slower than the
#               raw-buffer one. This is uDepot's first design principle stated
#               as a test rather than as a recorded number, and needs only one
#               build.
#
#    ab         io_layer_bench at the current revision against the change's
#               base revision, built in a worktree.
#
#  Usage:
#      bench/perf_test.sh [zerocopy|ab] ...     (default: both)
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
. "$HERE/../trt/bench/perflib.sh"

BENCH=bench/io_layer_bench
TARGETS=(bench/io_layer_bench)

# io_layer_bench needs >=200k ops for the comparison to mean anything: below
# that the PUT phase never becomes I/O bound and the result inverts at random.
OPS="${PERF_OPS:-200000}"

ROOT="$(pl_udepot_root)"
MODES=("$@")
[ ${#MODES[@]} -eq 0 ] && MODES=(zerocopy ab)
rc=0

has_mode() {
	local m
	for m in "${MODES[@]}"; do [ "$m" = "$1" ] && return 0; done
	return 1
}

echo "iterations: $PERF_ITERATIONS  threshold: $PERF_THRESHOLD  ops: $OPS"
echo

pl_info "building current revision ..."
pl_build "$ROOT" "${TARGETS[@]}"

# ---------------------------------------------------------------------------
# Zero-copy invariant: Mbuff must not be slower than raw buffers
# ---------------------------------------------------------------------------
if has_mode zerocopy; then
	if [ ! -x "$ROOT/$BENCH" ]; then
		echo "SKIP zerocopy: io_layer_bench not built"
	else
		for backend in aio uring; do
			# Probe first: a backend that cannot run at all should say so,
			# not drain away into "insufficient samples" and read as noise.
			if ! pl_probe "cd '$ROOT' && '$ROOT/$BENCH' --mbuff --$backend -n 1000"; then
				echo "SKIP zerocopy $backend: io_layer_bench $PL_PROBE_MSG"
				printf '%s\n' "$PL_PROBE_TAIL" | sed 's/^/    /'
				echo
				continue
			fi
			for phase in PUT GET; do
				for metric in kops mibs; do
					echo "TEST zerocopy $backend $phase [$metric]"
					out="$(pl_paired "$PERF_ITERATIONS" "$phase" "$metric" \
						"cd '$ROOT' && '$ROOT/$BENCH' --copy --$backend -n $OPS" \
						"cd '$ROOT' && '$ROOT/$BENCH' --mbuff --$backend -n $OPS")"
					printf '%s\n' "$out" | \
						pl_report "raw-buffer" "mbuff" "$PERF_THRESHOLD" improvement
					case $? in
						0) ;;
						77) echo "  SKIP: insufficient samples" ;;
						*) rc=1 ;;
					esac
					echo
				done
			done
		done
	fi
fi

# ---------------------------------------------------------------------------
# A/B against the base revision
# ---------------------------------------------------------------------------
if has_mode ab; then
	BASE_REV="$(pl_base_rev "$ROOT")"
	if [ -z "$BASE_REV" ]; then
		echo "SKIP ab: could not resolve a base revision (set PERF_BASE_REF)"
	elif [ "$BASE_REV" = "$(git -C "$ROOT" rev-parse HEAD)" ]; then
		echo "SKIP ab: HEAD is the base revision -- nothing to compare"
	else
		TMPDIR_BASE="$(mktemp -d -t udepot-perf-base-XXXXXX)"
		BASE_WT="$TMPDIR_BASE/udepot"
		cleanup() { pl_remove_base "$ROOT" "$BASE_WT"; rm -rf "$TMPDIR_BASE"; }
		trap cleanup EXIT

		echo "base:    $(git -C "$ROOT" rev-parse --short "$BASE_REV")"
		echo "current: $(git -C "$ROOT" rev-parse --short HEAD)"
		pl_info "building base revision ..."
		pl_prepare_base "$ROOT" "$BASE_REV" "$BASE_WT" "${TARGETS[@]}"

		for mode in mbuff copy; do
			for phase in PUT GET; do
				for metric in kops mibs; do
					echo "TEST ab io_layer_$mode-aio $phase [$metric]"
					out="$(pl_paired "$PERF_ITERATIONS" "$phase" "$metric" \
						"cd '$BASE_WT' && '$BASE_WT/$BENCH' --$mode --aio -n $OPS" \
						"cd '$ROOT' && '$ROOT/$BENCH' --$mode --aio -n $OPS")"
					printf '%s\n' "$out" | \
						pl_report base current "$PERF_THRESHOLD" regression
					case $? in
						0) ;;
						77) echo "  SKIP: insufficient samples" ;;
						*) rc=1 ;;
					esac
					echo
				done
			done
		done
	fi
fi

exit $rc
