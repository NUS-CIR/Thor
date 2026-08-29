#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo_dir"

image="${THOR_IMAGE:-thor-nfapi-proxy:release}"
run_tests=false
no_cache=false

usage() {
    cat <<'EOF'
Usage: ./build_image.sh [options]

Build the linux/amd64 Thor nFAPI proxy image.

Options:
  --tag IMAGE[:TAG]  Output image tag (default: thor-nfapi-proxy:release)
  --test             Run unit and two-L1 integration tests in Docker first
  --no-cache         Disable Docker build cache
  -h, --help         Show this help

The THOR_IMAGE environment variable can also set the output image tag.
EOF
}

while (($# > 0)); do
    case "$1" in
        --tag)
            [[ $# -ge 2 ]] || { echo "--tag requires a value" >&2; exit 2; }
            image="$2"
            shift 2
            ;;
        --test)
            run_tests=true
            shift
            ;;
        --no-cache)
            no_cache=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

docker_cmd=(docker)
if ! docker info >/dev/null 2>&1; then
    if sudo -n docker info >/dev/null 2>&1; then
        docker_cmd=(sudo docker)
    else
        echo "Cannot access the Docker daemon. Configure Docker access or run with sudo." >&2
        exit 1
    fi
fi

commit_hash="unknown"
commit_date="unknown"
if command -v git >/dev/null 2>&1 && git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    commit_hash="$(git rev-parse --short HEAD)"
    commit_date="$(git log -1 --format=%cd --date=format:'%Y-%m-%d %H:%M:%S')"
fi

build_args=(
    --platform linux/amd64
    --build-arg "THOR_GIT_COMMIT_HASH=${commit_hash}"
    --build-arg "THOR_GIT_COMMIT_DATE=${commit_date}"
)
if [[ "$no_cache" == true ]]; then
    build_args+=(--no-cache)
fi

if [[ "$run_tests" == true ]]; then
    test_image="${image}-test"
    echo "Building AMD64 test image: ${test_image}"
    "${docker_cmd[@]}" build "${build_args[@]}" --target test -t "$test_image" .
    echo "Running containerized unit and integration tests"
    "${docker_cmd[@]}" run --rm --platform linux/amd64 "$test_image"
fi

echo "Building AMD64 runtime image: ${image}"
"${docker_cmd[@]}" build "${build_args[@]}" --target runtime -t "$image" .
echo "Built ${image} for linux/amd64"
