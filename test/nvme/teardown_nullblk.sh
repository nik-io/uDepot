#!/usr/bin/env bash
# Remove null_blk kernel device(s) set up by setup_nullblk.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STATE_DIR="${1:-${SCRIPT_DIR}/.nullblk_state}"

if [[ $EUID -ne 0 ]]; then
    echo "ERROR: root required for rmmod. Run with sudo." >&2
    exit 1
fi

if lsmod | grep -q '^null_blk'; then
    echo "Removing null_blk module..."
    rmmod null_blk
    echo "null_blk removed."
else
    echo "null_blk not loaded."
fi

rm -rf "$STATE_DIR"
echo "Cleanup complete."
