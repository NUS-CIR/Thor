# Copyright (c) National University of Singapore.
# Licensed under the MIT License.
#
# Packages thor_fhaul_proxy and its test suite.
#
# Stages:
#   builder  - DPDK + patched fhi_lib + the proxy and unit test binaries
#   runtime  - just enough to run the proxy
#   test     - runtime plus python/scapy and the test suite
#
# Build from the repository root, not from docker/:
#     docker build -f docker/Dockerfile --target runtime -t ranbooster/thor-proxy .
#
# amd64 only: the IQ merge is written in AVX-512 intrinsics, so there is no
# other architecture to target. The platform is pinned on every stage so the
# build fails loudly rather than silently emulating on an arm64 host.
#
# The image is self-contained: it fetches DPDK and o-du/phy itself rather than
# relying on anything already present in the working tree.

ARG UBUNTU_VERSION=24.04
# amd64 only; declared as an ARG so buildkit does not warn about a constant
# --platform, and so a cross-build can be forced deliberately if ever needed.
ARG BUILD_PLATFORM=linux/amd64

# Drivers that must exist in the finished image. Checked in the runtime stage,
# so a typo or a missing build dependency fails the build instead of turning
# into "no such device" against real hardware. Declared here, before the first
# FROM, so that both stages can see it -- an ARG declared inside a stage is
# scoped to that stage and would read back empty elsewhere.
ARG REQUIRED_PMDS="af_packet e1000 i40e iavf ice ixgbe mlx5"

# ---------------------------------------------------------------------------
# builder
# ---------------------------------------------------------------------------
FROM --platform=${BUILD_PLATFORM} ubuntu:${UBUNTU_VERSION} AS builder

ARG DPDK_VERSION=24.11.2
# Pinned to the commit recorded for the 3p/phy submodule.
ARG PHY_COMMIT=6ef1d2b70db585e351b9cd35c6054c1b249a7465
# NOT "native": the proxy's IQ merge uses AVX-512, and a native build would
# SIGILL on any host narrower than the one that built the image. icelake-server
# matches what fhi_lib is built with (WIRELESS_SDK_TARGET_ISA=snc).
ARG TARGET_MARCH=icelake-server

# Drivers to build. The full DPDK set is ~150 plugins of which almost none are
# reachable here, so this covers the NICs actually in use -- Intel E810/E8xx
# (ice, iavf), X710/XL710 (i40e), 82599/X5xx (ixgbe), igb/em (e1000), and
# Mellanox CX-4..7 (mlx5) -- plus af_packet, which the veth tests run on.
# bus/mempool/common are pulled in wholesale because the net drivers depend on
# them; a missing one disables its dependants silently.
ARG DPDK_DRIVERS=bus/*,mempool/*,common/*,net/ice,net/iavf,net/i40e,net/ixgbe,net/e1000,net/mlx4,net/mlx5,net/af_packet,net/tap,net/null,net/ring

ENV DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates wget xz-utils \
        meson ninja-build pkg-config python3-pyelftools \
        libnuma-dev libbsd-dev libelf-dev \
    # rdma-core headers, without which meson silently skips mlx4/mlx5 and the
    # image would have no driver for the Mellanox deployment.
        libibverbs-dev librdmacm-dev libmnl-dev \
    && rm -rf /var/lib/apt/lists/*

# --- DPDK -------------------------------------------------------------------
# Ubuntu 24.04 ships DPDK 23.11; the proxy needs 24.11+, so build from source.
WORKDIR /opt
RUN mkdir -p dpdk \
    && wget -qO - "https://fast.dpdk.org/rel/dpdk-${DPDK_VERSION}.tar.xz" \
       | tar -xJ -C dpdk --strip-components=1 \
    && cd dpdk \
    && meson setup build --buildtype=release --prefix=/usr/local \
         -Denable_drivers="${DPDK_DRIVERS}" \
         | tee /opt/dpdk-meson-setup.log \
    && ninja -C build \
    && ninja -C build install \
    && ldconfig

# --- RANBooster -------------------------------------------------------------
WORKDIR /opt/ranbooster

# Only what the build needs, so edits to docs or config do not bust the cache.
COPY CMakeLists.txt setup_ranbooster_env.sh ./
COPY patches/ patches/
COPY src/ src/
COPY tests/unit/ tests/unit/

# Intel's fronthaul library, patched to build with gcc and to keep its O-RAN
# headers usable without DPDK. Equivalent to init_and_patch_submodules.sh, but
# cloning the pinned commit directly so no submodule state is needed.
RUN git clone --quiet https://gerrit.o-ran-sc.org/r/o-du/phy.git 3p/phy \
    && cd 3p/phy \
    && git checkout --quiet "${PHY_COMMIT}" \
    && git apply /opt/ranbooster/patches/ofh_lib.patch

ENV RTE_SDK=/opt/dpdk
RUN source ./setup_ranbooster_env.sh \
    && cmake -S . -B build \
         -DCMAKE_BUILD_TYPE=Release \
         -DBUILD_TESTING=ON \
         -DRANBOOSTER_MARCH="${TARGET_MARCH}" \
    && make -C build -j"$(nproc)" thor_fhaul_proxy_dpdk test_datapath

# ---------------------------------------------------------------------------
# runtime
# ---------------------------------------------------------------------------
FROM --platform=${BUILD_PLATFORM} ubuntu:${UBUNTU_VERSION} AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# libnuma/libbsd are linked directly; netcat lets thor_ctl.sh drive the control
# socket from inside the container.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libnuma1 libbsd0 libatomic1 netcat-openbsd iproute2 \
        libibverbs1 librdmacm1 libmnl0 ibverbs-providers \
    && rm -rf /var/lib/apt/lists/*

# DPDK runtime libraries and the PMD plugins EAL loads at startup.
COPY --from=builder /usr/local/lib/x86_64-linux-gnu/ /usr/local/lib/x86_64-linux-gnu/
COPY --from=builder /opt/dpdk-meson-setup.log /usr/local/share/dpdk-meson-setup.log
RUN ldconfig

# EAL dlopen()s every plugin in the PMD directory at startup and aborts on the
# first one that fails to load, so an unsatisfied dependency is fatal rather
# than a missing driver. Fail the build here instead of at run time.
RUN missing=$(for so in /usr/local/lib/x86_64-linux-gnu/dpdk/pmds-*/*.so; do \
                  ldd "$so" 2>/dev/null; \
              done | grep 'not found' | sort -u); \
    if [ -n "$missing" ]; then \
        echo "PMD plugins have unresolved dependencies:" >&2; \
        echo "$missing" >&2; \
        exit 1; \
    fi; \
    echo "all $(ls /usr/local/lib/x86_64-linux-gnu/dpdk/pmds-*/*.so | wc -l) PMD plugins resolve"

ARG REQUIRED_PMDS
RUN for pmd in ${REQUIRED_PMDS}; do \
        if ! ls /usr/local/lib/x86_64-linux-gnu/dpdk/pmds-*/librte_net_"$pmd".so >/dev/null 2>&1; then \
            echo "required PMD net_$pmd was not built" >&2; \
            echo "check /usr/local/share/dpdk-meson-setup.log for why" >&2; \
            exit 1; \
        fi; \
    done; \
    echo "required PMDs present: ${REQUIRED_PMDS}"

COPY --from=builder /opt/ranbooster/build/thor_fhaul_proxy_dpdk/thor_fhaul_proxy_dpdk \
                    /usr/local/bin/thor_fhaul_proxy_dpdk
COPY config/thor_ctl.sh /usr/local/bin/thor_ctl.sh

# Where the control socket lands by default; mount this to reach it from the
# host or from another container.
RUN mkdir -p /var/run/ranbooster
ENV THOR_CTRL_SOCK=/var/run/ranbooster/thor_fhaul_proxy.sock

ENTRYPOINT ["thor_fhaul_proxy_dpdk"]
# EAL swallows a bare --help, so running the image with no arguments would show
# DPDK's options rather than the proxy's. Put it behind the "--" separator with
# a minimal EAL setup so the default output is the proxy's own usage.
CMD ["--no-huge", "--no-pci", "-m", "64", "--", "--help"]

# ---------------------------------------------------------------------------
# test
# ---------------------------------------------------------------------------
FROM runtime AS test

ENV DEBIAN_FRONTEND=noninteractive

# libpcap is what scapy compiles its BPF capture filters through. Without it
# AsyncSniffer dies in its own thread and every capture silently yields nothing.
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 python3-venv ethtool procps libpcap0.8t64 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/ranbooster

COPY tests/ tests/
COPY --from=builder /opt/ranbooster/build/tests/test_datapath build/tests/test_datapath
# The integration harness looks for the proxy at this path inside the repo.
RUN mkdir -p build/thor_fhaul_proxy_dpdk \
    && ln -sf /usr/local/bin/thor_fhaul_proxy_dpdk \
              build/thor_fhaul_proxy_dpdk/thor_fhaul_proxy_dpdk

RUN python3 -m venv venv \
    && venv/bin/pip install --no-cache-dir --quiet -r tests/requirements.txt

# Runs unit tests, then the veth integration tests. Needs NET_ADMIN and NET_RAW
# to build the veth pair and open AF_PACKET sockets; see docker-compose.yml.
ENTRYPOINT ["tests/run_tests.sh"]
CMD []
