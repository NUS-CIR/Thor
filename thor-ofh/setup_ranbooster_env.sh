#!/bin/bash

# Copyright (c) Microsoft Corporation.
# Copyright (c) National University of Singapore.
# Licensed under the MIT License.
#
# Environment for building RANBooster. Must be *sourced*, not executed, so the
# exports land in the calling shell:
#
#     source setup_ranbooster_env.sh
#
# RTE_SDK is deliberately not set here -- export it yourself, pointing at your
# DPDK installation, before running any of the build scripts.

# Repo root, derived from this script's own location so it works from anywhere.
export RANBOOSTER_PATH="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd)"

# Where Intel's fronthaul interface library lives. Read by fhi_lib's own
# Makefile, which CMake shells out to (see the fhi_lib_make target).
export XRAN_DIR=$RANBOOSTER_PATH/3p/phy/fhi_lib

# Target instruction set for fhi_lib: "snc" (Sunny Cove) selects the AVX-512
# code paths and builds with -march=icelake-server. The middlebox's IQ merge
# also assumes AVX-512, so a host without it needs both changed.
export WIRELESS_SDK_TARGET_ISA=snc

# Compiler family for fhi_lib. Upstream only supports icc; the gcc branch is
# added by patches/ofh_lib.patch, so this must stay in step with that patch.
# fhi_lib's Makefile hard-fails if this is unset.
export WIRELESS_SDK_TOOLCHAIN=gcc
