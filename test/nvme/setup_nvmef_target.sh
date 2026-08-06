#!/usr/bin/env bash
# Set up an SPDK NVMeoF TCP target backed by a malloc bdev for testing.
#
# Prerequisites:
#   - SPDK built (make -C trt build_spdk)
#   - Hugepages configured (e.g., echo 1024 > /proc/sys/vm/nr_hugepages)
#   - Root or SPDK vfio-user permissions
#
# Usage:
#   ./setup_nvmef_target.sh [--spdk-dir DIR] [--addr IP] [--port PORT] [--size-mb MB]
#
# The script writes connection parameters to $STATE_DIR/nvmef_target.env
# which can be sourced by test scripts.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

SPDK_DIR="${REPO_ROOT}/trt/external/spdk"
LISTEN_ADDR="127.0.0.1"
LISTEN_PORT="4420"
BDEV_SIZE_MB=64
BDEV_BLOCK_SIZE=512
SUBNQN="nqn.2024-01.io.udepot:test-target"
STATE_DIR="${SCRIPT_DIR}/.nvmef_state"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --spdk-dir)  SPDK_DIR="$2"; shift 2;;
        --addr)      LISTEN_ADDR="$2"; shift 2;;
        --port)      LISTEN_PORT="$2"; shift 2;;
        --size-mb)   BDEV_SIZE_MB="$2"; shift 2;;
        --state-dir) STATE_DIR="$2"; shift 2;;
        -h|--help)
            sed -n '2,/^$/s/^# //p' "$0"
            exit 0;;
        *) echo "Unknown option: $1" >&2; exit 1;;
    esac
done

RPC="${SPDK_DIR}/scripts/rpc.py"
NVMF_TGT="${SPDK_DIR}/build/bin/nvmf_tgt"

for f in "$RPC" "$NVMF_TGT"; do
    if [[ ! -x "$f" ]]; then
        echo "ERROR: $f not found or not executable." >&2
        echo "Build SPDK first: make -C trt build_spdk" >&2
        exit 1
    fi
done

mkdir -p "$STATE_DIR"

echo "Starting nvmf_tgt..."
"$NVMF_TGT" -m 0x1 &
NVMF_PID=$!
echo "$NVMF_PID" > "${STATE_DIR}/nvmf_tgt.pid"

cleanup() {
    echo "Setup failed, cleaning up..."
    kill "$NVMF_PID" 2>/dev/null || true
    rm -rf "$STATE_DIR"
    exit 1
}
trap cleanup ERR

echo "Waiting for RPC server..."
for i in $(seq 1 30); do
    if "$RPC" rpc_get_methods >/dev/null 2>&1; then
        break
    fi
    if ! kill -0 "$NVMF_PID" 2>/dev/null; then
        echo "ERROR: nvmf_tgt exited unexpectedly" >&2
        exit 1
    fi
    sleep 1
done

if ! "$RPC" rpc_get_methods >/dev/null 2>&1; then
    echo "ERROR: RPC server did not become ready" >&2
    cleanup
fi

echo "Creating malloc bdev (${BDEV_SIZE_MB}MB, ${BDEV_BLOCK_SIZE}B blocks)..."
"$RPC" bdev_malloc_create -b Malloc0 "$BDEV_SIZE_MB" "$BDEV_BLOCK_SIZE"

echo "Creating NVMeoF TCP transport..."
"$RPC" nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192

echo "Creating NVMeoF subsystem: ${SUBNQN}"
"$RPC" nvmf_create_subsystem "$SUBNQN" -a -s UDEPOTTEST0001

echo "Adding namespace..."
"$RPC" nvmf_subsystem_add_ns "$SUBNQN" Malloc0

echo "Adding TCP listener on ${LISTEN_ADDR}:${LISTEN_PORT}..."
"$RPC" nvmf_subsystem_add_listener "$SUBNQN" -t tcp -a "$LISTEN_ADDR" -s "$LISTEN_PORT"

cat > "${STATE_DIR}/nvmef_target.env" <<ENVEOF
NVMEF_TRANSPORT=TCP
NVMEF_ADDR=${LISTEN_ADDR}
NVMEF_PORT=${LISTEN_PORT}
NVMEF_SUBNQN=${SUBNQN}
NVMEF_PID=${NVMF_PID}
NVMEF_SPDK_DIR=${SPDK_DIR}
ENVEOF

trap - ERR

echo ""
echo "NVMeoF TCP target ready:"
echo "  Address:   ${LISTEN_ADDR}:${LISTEN_PORT}"
echo "  SubNQN:    ${SUBNQN}"
echo "  Bdev:      Malloc0 (${BDEV_SIZE_MB}MB)"
echo "  PID:       ${NVMF_PID}"
echo "  State:     ${STATE_DIR}/nvmef_target.env"
