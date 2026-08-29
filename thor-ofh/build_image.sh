#!/bin/bash

# Copyright (c) National University of Singapore.
# Licensed under the MIT License.
#
# Builds the thor_fhaul_proxy container images.
#
#     ./build_image.sh              # runtime image
#     ./build_image.sh test         # test image (adds python, scapy, the suite)
#     ./build_image.sh all          # both
#     ./build_image.sh test --run   # build the test image and run the suite
#
# Unlike build_ranbooster.sh this needs no RTE_SDK and no submodule: the image
# fetches DPDK and o-du/phy itself. Only docker is required.
#
# Overridable:
#   IMAGE         image name           (default ranbooster/thor-proxy)
#   TAG           tag for the runtime image  (default latest)
#   DPDK_VERSION  DPDK release to build (default 24.11.2)
#   TARGET_MARCH  -march= for the proxy (default icelake-server)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

IMAGE="${IMAGE:-ranbooster/thor-proxy}"
TAG="${TAG:-latest}"

TARGET="${1:-runtime}"
RUN_TESTS=0
[ $# -gt 0 ] && shift || true
for arg in "$@"; do
    case "$arg" in
        --run) RUN_TESTS=1 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

if ! command -v docker >/dev/null 2>&1; then
    echo "error: docker is not installed or not on PATH" >&2
    exit 1
fi

build() {
    local target=$1 tag=$2
    echo "==> building ${IMAGE}:${tag}  (target: ${target})"
    # Built from the repository root: the Dockerfile copies src/, tests/ and
    # patches/ from the build context.
    docker build \
        --platform linux/amd64 \
        -f docker/Dockerfile \
        --target "$target" \
        ${DPDK_VERSION:+--build-arg DPDK_VERSION="$DPDK_VERSION"} \
        ${TARGET_MARCH:+--build-arg TARGET_MARCH="$TARGET_MARCH"} \
        -t "${IMAGE}:${tag}" \
        .
}

case "$TARGET" in
    runtime) build runtime "$TAG" ;;
    test)    build test test ;;
    all)     build runtime "$TAG"; build test test ;;
    *)       echo "usage: $0 [runtime|test|all] [--run]" >&2; exit 2 ;;
esac

if [ "$RUN_TESTS" -eq 1 ]; then
    if [ "$TARGET" = "runtime" ]; then
        echo "error: --run needs the test image; use '$0 test --run'" >&2
        exit 2
    fi
    echo
    echo "==> running the test suite in ${IMAGE}:test"
    # NET_ADMIN/NET_RAW build the veth pair and open AF_PACKET sockets;
    # IPC_LOCK is for EAL's memory locking. The veth lives in the container's
    # own network namespace, so this touches nothing on the host.
    docker run --rm \
        --platform linux/amd64 \
        --cap-add NET_ADMIN \
        --cap-add NET_RAW \
        --cap-add IPC_LOCK \
        --shm-size 1g \
        "${IMAGE}:test"
fi

echo
echo "==> done"
docker images "${IMAGE}" --format '    {{.Repository}}:{{.Tag}}  {{.Size}}'
