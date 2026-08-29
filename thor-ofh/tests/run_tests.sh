#!/bin/bash
#
# Copyright (c) National University of Singapore.
# Licensed under the MIT License.
#
# Runs the thor_fhaul_proxy test suite.
#
#   tests/run_tests.sh            # unit tests, then veth integration tests
#   tests/run_tests.sh unit       # unit tests only (no root needed)
#   tests/run_tests.sh integration [pytest args...]
#
# The integration tests create veth interfaces and open AF_PACKET sockets, so
# they re-exec themselves under sudo.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
VENV_PY="$REPO_ROOT/venv/bin/python"
UNIT_BIN="$BUILD_DIR/tests/test_datapath"

WHAT="${1:-all}"
[ $# -gt 0 ] && shift || true

run_unit() {
    if [ ! -x "$UNIT_BIN" ]; then
        echo "error: $UNIT_BIN not built. Run tests/build.sh first." >&2
        exit 1
    fi
    echo "=== unit tests ==="
    # --no-huge keeps these runnable without hugepage setup, and --no-pci stops
    # EAL probing NICs another application may be using.
    "$UNIT_BIN" --no-huge -m 512 --no-pci --no-telemetry --file-prefix thor_unit
}

run_integration() {
    if [ ! -x "$VENV_PY" ]; then
        echo "error: $VENV_PY missing. Create it with:" >&2
        echo "    python3 -m venv venv && venv/bin/pip install -r tests/requirements.txt" >&2
        exit 1
    fi
    echo "=== integration tests (veth) ==="
    if [ "$(id -u)" -ne 0 ]; then
        exec sudo -E "$VENV_PY" -m pytest -c "$REPO_ROOT/tests/pytest.ini" \
             --rootdir "$REPO_ROOT/tests" "$REPO_ROOT/tests/integration" "$@"
    fi
    "$VENV_PY" -m pytest -c "$REPO_ROOT/tests/pytest.ini" \
        --rootdir "$REPO_ROOT/tests" "$REPO_ROOT/tests/integration" "$@"
}

case "$WHAT" in
    unit)        run_unit ;;
    integration) run_integration "$@" ;;
    all)         run_unit; echo; run_integration "$@" ;;
    *)           echo "usage: $0 [unit|integration|all] [pytest args...]" >&2; exit 2 ;;
esac
