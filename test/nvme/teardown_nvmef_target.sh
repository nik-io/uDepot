#!/usr/bin/env bash
# Tear down the SPDK NVMeoF TCP target started by setup_nvmef_target.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STATE_DIR="${1:-${SCRIPT_DIR}/.nvmef_state}"

if [[ ! -f "${STATE_DIR}/nvmef_target.env" ]]; then
    echo "No active NVMeoF target found (missing ${STATE_DIR}/nvmef_target.env)"
    exit 0
fi

# shellcheck source=/dev/null
source "${STATE_DIR}/nvmef_target.env"

if [[ -n "${NVMEF_PID:-}" ]] && kill -0 "$NVMEF_PID" 2>/dev/null; then
    echo "Stopping nvmf_tgt (PID ${NVMEF_PID})..."
    kill "$NVMEF_PID"
    for i in $(seq 1 10); do
        kill -0 "$NVMEF_PID" 2>/dev/null || break
        sleep 1
    done
    if kill -0 "$NVMEF_PID" 2>/dev/null; then
        echo "Force killing nvmf_tgt..."
        kill -9 "$NVMEF_PID" 2>/dev/null || true
    fi
    echo "nvmf_tgt stopped."
else
    echo "nvmf_tgt not running (PID ${NVMEF_PID:-unknown})."
fi

rm -rf "$STATE_DIR"
echo "Cleanup complete."
