#!/usr/bin/env bash
#
# End-to-end test of the TRT SPDK backend against a software NVMe-over-Fabrics
# target -- no NVMe hardware required.
#
# It starts an SPDK nvmf_tgt that exports a RAM-backed malloc bdev as an NVMe
# namespace over TCP loopback, then runs udepot-test's SPDK backend (-u 7) as a
# fabrics initiator against it and checks that PUTs/GETs complete and the store
# shuts down cleanly. This is the first test that actually exercises trt::SPDK,
# SpdkQpair and the TRT scheduler on the I/O path (the spdk_bdev_* smoke tests do
# not -- see docs/TODO-spdk-testing.md).
#
# The SPDK backend learns the target from the UDEPOT_NVMEF environment variable
# (traddr:trsvcid:subnqn), so udepot-test needs no command-line change.
#
# Requirements: a BUILD_SPDK=1 build of bin/udepot-test, a built SPDK tree under
# trt/external/spdk, hugepages, and root (for hugepages and the target). CI runs
# it under sudo.
#
# Usage: scripts/spdk-nvmef-test.sh [ops]
set -uo pipefail

OPS="${1:-20000}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SPDK_DIR="$HERE/trt/external/spdk"
RPC="$SPDK_DIR/scripts/rpc.py"
TGT_BIN="$SPDK_DIR/build/bin/nvmf_tgt"
DPDK_LIB="$SPDK_DIR/dpdk/build/lib"
UDEPOT_TEST="$HERE/bin/udepot-test"

NQN="nqn.2016-06.io.spdk:cnode1"
TADDR="127.0.0.1"
TPORT="4420"
# 513 MiB, deliberately NOT a multiple of the segment size: uDepot puts device
# metadata in the tail after align_down(dev, seg*grain), so an exactly divisible
# size leaves no room and init fails with "Not enough spare capacity".
BDEV_MB="513"
SECTOR="512"

TGT_LOG="$(mktemp /tmp/nvmf_tgt.XXXXXX.log)"
TGT_PID=""
HUGE_PREEXISTING="no"

log() { echo "[spdk-nvmef-test] $*"; }
fail() { echo "[spdk-nvmef-test] FAIL: $*" >&2; exit 1; }

cleanup() {
    local rc=$?
    [ -n "$TGT_PID" ] && kill "$TGT_PID" 2>/dev/null
    # give it a moment, then hard-kill
    for _ in 1 2 3 4 5; do kill -0 "$TGT_PID" 2>/dev/null || break; sleep 0.3; done
    kill -9 "$TGT_PID" 2>/dev/null
    pkill -9 -f "nvmf_tgt" 2>/dev/null
    if [ "$HUGE_PREEXISTING" = "no" ]; then
        echo 0 > /proc/sys/vm/nr_hugepages 2>/dev/null || true
    fi
    [ $rc -ne 0 ] && [ -f "$TGT_LOG" ] && { echo "--- target log tail ---" >&2; tail -20 "$TGT_LOG" >&2; }
    rm -f "$TGT_LOG"
    exit $rc
}
trap cleanup EXIT INT TERM

[ -x "$UDEPOT_TEST" ] || fail "$UDEPOT_TEST not found -- build with 'make BUILD_SPDK=1'"
[ -x "$TGT_BIN" ]     || fail "$TGT_BIN not found -- build SPDK first"

# ── hugepages ────────────────────────────────────────────────────────────────
CUR_HUGE="$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0)"
if [ "$CUR_HUGE" -ge 512 ]; then
    HUGE_PREEXISTING="yes"
else
    log "reserving hugepages"
    echo 1024 > /proc/sys/vm/nr_hugepages || fail "cannot reserve hugepages (need root)"
fi
if ! mount | grep -q 'hugetlbfs'; then
    mkdir -p /dev/hugepages
    mount -t hugetlbfs nodev /dev/hugepages || fail "cannot mount hugetlbfs"
fi

# ── start the target (pinned to core 0) ─────────────────────────────────────
log "starting nvmf_tgt"
"$TGT_BIN" -m 0x1 > "$TGT_LOG" 2>&1 &
TGT_PID=$!
# Wait until the RPC server actually answers, not just until the socket file
# exists: the socket appears before the reactor is ready, and a too-early
# nvmf_create_transport fails.
ready="no"
for i in $(seq 1 40); do
    kill -0 "$TGT_PID" 2>/dev/null || fail "nvmf_tgt exited during startup"
    if test -S /var/tmp/spdk.sock && "$RPC" spdk_get_version >/dev/null 2>&1; then
        ready="yes"; break
    fi
    sleep 1
done
[ "$ready" = "yes" ] || fail "nvmf_tgt RPC did not become ready"

# ── configure: TCP transport, malloc bdev, subsystem, listener ──────────────
log "configuring target ($BDEV_MB MiB malloc bdev over TCP $TADDR:$TPORT)"
"$RPC" nvmf_create_transport -t TCP                              || fail "create_transport"
"$RPC" bdev_malloc_create "$BDEV_MB" "$SECTOR" -b Malloc0        || fail "bdev_malloc_create"
"$RPC" nvmf_create_subsystem "$NQN" -a -s SPDK00000000000001    || fail "create_subsystem"
"$RPC" nvmf_subsystem_add_ns "$NQN" Malloc0                      || fail "add_ns"
"$RPC" nvmf_subsystem_add_listener "$NQN" -t tcp -a "$TADDR" -s "$TPORT" || fail "add_listener"

# ── run udepot-test SPDK backend as the fabrics initiator ───────────────────
# Keep the initiator off the target's core 0 when there are enough cores; the
# SPDK backend needs >= 2 lcores (one scheduler worker + the DPDK main lcore).
NCPU="$(nproc)"
PIN=()
if [ "$NCPU" -ge 3 ]; then
    PIN=(taskset -c "1-$((NCPU-1))")
fi
log "running udepot-test SPDK backend ($OPS ops) on ${NCPU} cpus"
# grain 4096 (a realistic device page/sector), segment 4096 grains = 16 MiB.
# The segment must be small enough that the 513 MiB device yields >= 4 GC-spare
# segments (SALSA's minimum); at 16 MiB that is 32 segments, comfortably above
# the minimum. A larger segment (e.g. 32 MiB) is silently right-sized down by
# uDepot on a device this small -- see udepot-lsa.cc's segment-size fallback.
UDEPOT_NVMEF="$TADDR:$TPORT:$NQN" LD_LIBRARY_PATH="$DPDK_LIB" \
    "${PIN[@]}" "$UDEPOT_TEST" -f 'SPDK' \
        -w "$OPS" -r "$OPS" -t 1 --grain-size 4096 --segment-size 4096 --val-size 3072 \
        -u 7 --thin --force-destroy
rc=$?
[ $rc -eq 0 ] || fail "udepot-test SPDK run failed (rc=$rc)"

# keep-alive timeouts on the target mean the initiator stopped polling the admin
# queue: the fabrics connection was dropped. A correct run has none.
if grep -q "keep alive timeout" "$TGT_LOG"; then
    fail "target reported a keep-alive timeout (initiator stopped polling)"
fi

log "OK: SPDK backend PUT/GET/shutdown succeeded over the soft NVMe-oF target"
