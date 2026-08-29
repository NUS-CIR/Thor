#!/bin/bash
#
# Copyright (c) National University of Singapore.
# Licensed under the MIT License.
#
# Incremental build of just what the test suite needs: the proxy binary and the
# unit test binary.
#
#     export RTE_SDK=~/dpdk-stable-24.11.2/
#     tests/build.sh
#
# Unlike build_ranbooster.sh this does NOT touch the 3p/phy submodule, so it is
# safe to re-run while iterating. Use build_ranbooster.sh for a clean checkout,
# or whenever patches/ofh_lib.patch changes.
#
# Override the build type with CMAKE_BUILD_TYPE, e.g. for a debuggable proxy:
#     CMAKE_BUILD_TYPE=Debug tests/build.sh

set -euo pipefail

# Run from the repo root regardless of where the script was invoked from.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# RTE_SDK must be exported by the caller, pointing at the DPDK installation.
if [ -z "${RTE_SDK:-}" ]; then
    echo "RTE_SDK is not set. Please export it, pointing at the DPDK installation path." >&2
    exit 1
fi

# Exports RANBOOSTER_PATH, XRAN_DIR and the WIRELESS_SDK_* variables that
# fhi_lib's Makefile needs; without them that Makefile refuses to run.
source ./setup_ranbooster_env.sh

# -DBUILD_TESTING=ON adds the tests/unit subdirectory, which is what provides
# the test_datapath target.
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" -DBUILD_TESTING=ON ..
make -j"$(nproc)" thor_fhaul_proxy_dpdk test_datapath
