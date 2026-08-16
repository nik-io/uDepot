#!/usr/bin/env bash
#
# Zero-copy GET invariant, driven by udepot-test (the same binary the functional
# tests use). The Mbuff (zero-copy) KV interface must not be slower than the
# raw-buffer (copy) one on GET.
#
# Why GET and why udepot-test:
#   - GET is cache-bound, so the value memcpy the zero-copy path avoids is a
#     real, consistent win (~+6-12%). PUT is I/O bound -- the same memcpy is
#     below write-latency noise and its delta flips sign run to run -- so we do
#     not gate on it.
#   - --thin            selects the copy interface   (get_test_thin)
#   - --thin --zero-copy selects the zero-copy interface (get_test_thin_mbuff)
#   The two runs are interleaved and compared by median, which cancels the slow
#   throughput drift a shared CI runner has.
#
# Usage: perf-zerocopy.sh <u-code> [ops] [iters]
#   <u-code>  5 = SALSA_TRT_AIO, 6 = SALSA_TRT_URING
set -uo pipefail

UCODE="${1:?usage: perf-zerocopy.sh <u-code> [ops] [iters]}"
OPS="${2:-10000}"
ITERS="${3:-5}"
SIZE=1077936129            # (1048576+4096)*1024+1
BIN=bin/udepot-test
FILE="/tmp/udepot-perf-u${UCODE}.store"

get_mops() {  # $1 = extra flags ("" for copy, "--zero-copy" for zero-copy)
    rm -f "$FILE"
    "$BIN" -u "$UCODE" -f "$FILE" -w "$OPS" -r "$OPS" --size "$SIZE" -t 1 \
        --trt-ntasks 32 --force-destroy --grain-size 512 --thin $1 2>/dev/null \
        | grep 'GETs aggregate' | grep -oE 'Mops/sec=[0-9.]+' | cut -d= -f2
}

median() { printf '%s\n' "$@" | sort -n | \
    awk '{a[NR]=$1} END{n=NR; if(n==0){print 0} else if(n%2){print a[(n+1)/2]} else {printf "%.6f",(a[n/2]+a[n/2+1])/2}}'; }

copy=(); zc=()
for _ in $(seq 1 "$ITERS"); do
    c=$(get_mops "");            [ -n "$c" ] || { echo "copy run produced no GET throughput" >&2; exit 2; }
    z=$(get_mops "--zero-copy"); [ -n "$z" ] || { echo "zero-copy run produced no GET throughput" >&2; exit 2; }
    copy+=("$c"); zc+=("$z")
done

cm=$(median "${copy[@]}"); zm=$(median "${zc[@]}")
# Report to stderr so do_run_test surfaces it (it swallows stdout on success).
{
  echo "u${UCODE} GET Mops/sec over ${ITERS} interleaved runs @ ${OPS} ops:"
  echo "  copy median=${cm}   (${copy[*]})"
  echo "  zero-copy median=${zm}   (${zc[*]})"
} >&2

awk -v c="$cm" -v z="$zm" 'BEGIN{
    d = (c>0)? (z-c)/c*100 : 0;
    printf("  delta=%+.1f%%\n", d) > "/dev/stderr";
    if (z+0 < c+0) { print "FAIL: zero-copy GET is slower than copy" > "/dev/stderr"; exit 1; }
    print "OK: zero-copy GET >= copy" > "/dev/stderr"; exit 0;
}'
