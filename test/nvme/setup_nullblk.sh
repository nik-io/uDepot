#!/usr/bin/env bash
# Set up a null_blk kernel device for testing I/O backends without real NVMe.
#
# null_blk provides a /dev/nullb* block device backed by nothing — reads
# return zeros, writes are discarded.  It supports O_DIRECT and io_uring,
# making it suitable for testing uDepot's non-SPDK I/O backends on CI
# machines without NVMe hardware.
#
# Prerequisites:
#   - Linux kernel with null_blk module (CONFIG_BLK_DEV_NULL_BLK=m or y)
#   - Root or sudo access (modprobe + device permissions)
#
# Usage:
#   ./setup_nullblk.sh [--size-gb GB] [--block-size BYTES] [--nr-devices N]
#
# Writes device path(s) to $STATE_DIR/nullblk.env.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SIZE_GB=1
BLOCK_SIZE=4096
NR_DEVICES=1
STATE_DIR="${SCRIPT_DIR}/.nullblk_state"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --size-gb)     SIZE_GB="$2"; shift 2;;
        --block-size)  BLOCK_SIZE="$2"; shift 2;;
        --nr-devices)  NR_DEVICES="$2"; shift 2;;
        --state-dir)   STATE_DIR="$2"; shift 2;;
        -h|--help)
            sed -n '2,/^$/s/^# //p' "$0"
            exit 0;;
        *) echo "Unknown option: $1" >&2; exit 1;;
    esac
done

if [[ $EUID -ne 0 ]]; then
    echo "ERROR: root required for modprobe. Run with sudo." >&2
    exit 1
fi

if lsmod | grep -q '^null_blk'; then
    echo "null_blk already loaded — removing first..."
    rmmod null_blk 2>/dev/null || true
    sleep 1
fi

echo "Loading null_blk: ${NR_DEVICES} device(s), ${SIZE_GB}GB each, ${BLOCK_SIZE}B blocks..."
modprobe null_blk nr_devices="$NR_DEVICES" gb="$SIZE_GB" bs="$BLOCK_SIZE"

sleep 1

mkdir -p "$STATE_DIR"

DEVICES=""
for i in $(seq 0 $((NR_DEVICES - 1))); do
    DEV="/dev/nullb${i}"
    if [[ ! -b "$DEV" ]]; then
        echo "ERROR: expected ${DEV} not found after modprobe" >&2
        rmmod null_blk 2>/dev/null || true
        exit 1
    fi
    DEVICES="${DEVICES:+${DEVICES}:}${DEV}"
    echo "  ${DEV} ready (${SIZE_GB}GB, ${BLOCK_SIZE}B blocks)"
done

cat > "${STATE_DIR}/nullblk.env" <<ENVEOF
NULLBLK_DEVICES=${DEVICES}
NULLBLK_NR_DEVICES=${NR_DEVICES}
NULLBLK_SIZE_GB=${SIZE_GB}
NULLBLK_BLOCK_SIZE=${BLOCK_SIZE}
ENVEOF

echo ""
echo "null_blk ready:"
echo "  Devices: ${DEVICES}"
echo "  State:   ${STATE_DIR}/nullblk.env"
