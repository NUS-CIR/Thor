#! /bin/bash

# Copyright (c) National University of Singapore.
# Licensed under the MIT License.
#
# Full build from a clean checkout: fetch and patch the o-du/phy submodule,
# then build everything through CMake.
#
#     export RTE_SDK=~/dpdk-stable-24.11.2/
#     ./build_ranbooster.sh
#
# NOTE: this re-runs init_and_patch_submodules.sh every time, which DELETES and
# re-clones 3p/phy. For an incremental rebuild after editing middlebox sources,
# skip this script and just run `make -j` in build/ (or use tests/build.sh).

# We assume a system-wide installation of DPDK 24.11+ is available and that
# RTE_SDK has been exported to point at it. No default is applied: guessing the
# DPDK path silently builds against the wrong headers.
if [ -z "$RTE_SDK" ]; then
    echo "RTE_SDK is not set. Please export it, pointing at the DPDK installation path." >&2
    exit 1
fi

# Exports RANBOOSTER_PATH, XRAN_DIR and the WIRELESS_SDK_* variables that
# fhi_lib's Makefile needs. Sourced, so they reach the commands below.
source ./setup_ranbooster_env.sh

# Clone o-du/phy and apply patches/ofh_lib.patch (gcc support, DPDK-optional
# headers). Destructive -- see the warning in that script.
./init_and_patch_submodules.sh

# CMake drives two things: a custom target that shells out to fhi_lib's own
# Makefile to produce libxran.a, and the middlebox targets that link against it.
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release .. && make -j
