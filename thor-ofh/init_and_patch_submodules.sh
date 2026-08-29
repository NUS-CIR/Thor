#!/bin/bash

# Copyright (c) Microsoft Corporation.
# Copyright (c) National University of Singapore.
# Licensed under the MIT License.
#
# Fetches Intel's o-du/phy submodule and patches it so it builds here.
#
# patches/ofh_lib.patch does two things:
#   * teaches fhi_lib's Makefile to build with gcc (upstream assumes icc), and
#     drops icc-only flags such as -fasm-blocks;
#   * makes the O-RAN packet headers usable without DPDK -- the rte_* includes
#     go behind #ifdef DPDK_ENABLED and __rte_packed becomes
#     __attribute__((packed)) -- which is what lets src/lib/ranbooster_common.h
#     pull in xran_pkt_up.h / xran_pkt_cp.h from a BPF/XDP target as well.
#
# WARNING: this DELETES 3p/phy and re-clones it. Any local edits or build
# artifacts under 3p/phy are lost. The wipe is deliberate -- `git apply` is not
# idempotent, so re-running over an already-patched tree would fail. Note that
# build_ranbooster.sh calls this on every run.
#
# Requires RANBOOSTER_PATH; source setup_ranbooster_env.sh first.

set -euo pipefail

# Guard the rm -rf below: with RANBOOSTER_PATH unset it would expand to /3p/phy.
if [ -z "${RANBOOSTER_PATH:-}" ]; then
    echo "RANBOOSTER_PATH is not set. Run 'source setup_ranbooster_env.sh' first." >&2
    exit 1
fi

rm -rf "$RANBOOSTER_PATH/3p/phy"
git submodule update --init --recursive

cp "$RANBOOSTER_PATH/patches/ofh_lib.patch" "$RANBOOSTER_PATH/3p/phy"
cd "$RANBOOSTER_PATH/3p/phy"
git apply ofh_lib.patch
