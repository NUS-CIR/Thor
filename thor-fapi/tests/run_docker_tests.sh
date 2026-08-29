#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_dir"

ctest --test-dir build --output-on-failure
bash tests/run_tests.sh --duration "${NFAPI_TEST_DURATION:-3}" --build-dir build
