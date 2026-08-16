#!/usr/bin/env bash
#
# Zero-copy invariant, driven by udepot-test (the same binary the functional
# tests use). The Mbuff (zero-copy) KV interface must not be slower than the
# raw-buffer (copy) one, on PUT and GET.
#
# Why udepot-test, why /dev/shm, why both phases:
#   - --thin            selects the copy interface   (put/get_test_thin)
#   - --thin --zero-copy selects the zero-copy interface (put/get_test_thin_mbuff)
#     The only difference is the value memcpy the zero-copy path avoids.
#   - The store lives on /dev/shm (tmpfs, RAM-backed, buffered), so BOTH PUT and
#     GET are cache-bound: the avoided memcpy is a real, consistent win. On a
#     real disk with O_DIRECT the ops are I/O bound and that ~2% saving sits
#     below device noise, so the delta flips sign run to run (that is why the
#     earlier CI, on a normal device, failed on the uring GET phase).
#   - The copy and zero-copy runs are interleaved and compared by median, which
#     cancels the slow throughput drift a shared CI runner has. A small tolerance
#     absorbs residual per-pair noise; a real zero-copy regression is far larger.
#
# Usage: perf-zerocopy.sh <u-code> [ops] [iters] [tolerance-percent]
#   <u-code>  5 = SALSA_TRT_AIO, 6 = SALSA_TRT_URING
set -uo pipefail

UCODE="${1:?usage: perf-zerocopy.sh <u-code> [ops] [iters] [tol%]}"
OPS="${2:-10000}"
ITERS="${3:-9}"
TOL="${4:-5}"              # zero-copy may be at most TOL% slower (noise band)
SIZE=1077936129           # (1048576+4096)*1024+1
BIN=bin/udepot-test
FILE="/dev/shm/udepot-perf-u${UCODE}.store"
trap 'rm -f "$FILE"' EXIT   # do not leave a ~1GB store behind on /dev/shm

# Run once; echo "PUT_mops GET_mops".
run_one() {  # $1 = extra flags ("" for copy, "--zero-copy" for zero-copy)
    rm -f "$FILE"
    local out
    out=$("$BIN" -u "$UCODE" -f "$FILE" -w "$OPS" -r "$OPS" --size "$SIZE" -t 1 \
        --trt-ntasks 32 --force-destroy --grain-size 512 --thin $1 2>/dev/null)
    local p g
    p=$(grep 'PUTs aggregate' <<<"$out" | grep -oE 'Mops/sec=[0-9.]+' | cut -d= -f2)
    g=$(grep 'GETs aggregate' <<<"$out" | grep -oE 'Mops/sec=[0-9.]+' | cut -d= -f2)
    echo "$p $g"
}

median() { printf '%s\n' "$@" | sort -n | \
    awk '{a[NR]=$1} END{n=NR; if(n==0){print 0} else if(n%2){print a[(n+1)/2]} else {printf "%.6f",(a[n/2]+a[n/2+1])/2}}'; }

copy_p=(); copy_g=(); zc_p=(); zc_g=()
for _ in $(seq 1 "$ITERS"); do
    read -r cp cg <<<"$(run_one "")"
    read -r zp zg <<<"$(run_one "--zero-copy")"
    { [ -n "$cp" ] && [ -n "$cg" ] && [ -n "$zp" ] && [ -n "$zg" ]; } \
        || { echo "a udepot-test run produced no PUT/GET throughput" >&2; exit 2; }
    copy_p+=("$cp"); copy_g+=("$cg"); zc_p+=("$zp"); zc_g+=("$zg")
done

cpm=$(median "${copy_p[@]}"); zpm=$(median "${zc_p[@]}")
cgm=$(median "${copy_g[@]}"); zgm=$(median "${zc_g[@]}")

{
  echo "u${UCODE} Mops/sec over ${ITERS} interleaved runs @ ${OPS} ops on /dev/shm:"
  echo "  PUT copy median=${cpm}  zero-copy median=${zpm}"
  echo "  GET copy median=${cgm}  zero-copy median=${zgm}"
} >&2

# Gate each phase: fail if zero-copy is more than TOL% slower than copy.
check() {  # copy_median zero_median phase-name
    awk -v c="$1" -v z="$2" -v tol="$TOL" -v ph="$3" 'BEGIN{
        d = (c>0)? (z-c)/c*100 : 0;
        printf("  %s delta=%+.1f%% (fail if worse than -%.1f%%)\n", ph, d, tol) > "/dev/stderr";
        if (d < -tol) { printf("FAIL: %s zero-copy is >%.1f%% slower than copy\n", ph, tol) > "/dev/stderr"; exit 1; }
        exit 0;
    }'
}

rc=0
check "$cpm" "$zpm" PUT || rc=1
check "$cgm" "$zgm" GET || rc=1
if [ "$rc" -eq 0 ]; then echo "OK: zero-copy within tolerance of copy on PUT and GET" >&2; fi
exit "$rc"
