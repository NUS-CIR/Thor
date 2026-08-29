#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/../.." && pwd)"
build_dir="${repo_dir}/build-microbenchmark"
duration=5
output="/tmp/proxy_message_log.csv"
result=""
iterations=100000000
control_socket=/tmp/nfapi_proxy_control.sock
payload_size=1024
ue_count=3
seed="$(date +%s)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        --duration) duration="$2"; shift 2 ;;
        --output) output="$2"; shift 2 ;;
        --result) result="$2"; shift 2 ;;
        --payload-size) payload_size="$2"; shift 2 ;;
        --ues) ue_count="$2"; shift 2 ;;
        --seed) seed="$2"; shift 2 ;;
        --help)
            echo "Usage: $0 [--build-dir DIR] [--duration SECONDS] [--output CSV] [--result JSON] [--payload-size BYTES] [--ues COUNT] [--seed INTEGER]"
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$result" ]]; then result="${output%.csv}_summary.json"; fi

if ! [[ "$duration" =~ ^[1-9][0-9]*$ && "$payload_size" =~ ^[1-9][0-9]*$ &&
        "$ue_count" =~ ^[1-9][0-9]*$ && "$seed" =~ ^[0-9]+$ ]]; then
    echo "Duration, payload size, UE count, and seed must be positive integers" >&2
    exit 2
fi
if (( ue_count > 16 || payload_size * ue_count > 60 * 1024 )); then
    echo "UE count must be <= 16 and aggregate per-message payload <= 60 KiB" >&2
    exit 2
fi

selected_index=$(( (seed * 1103515245 + 12345) % ue_count ))
selected_rnti=$(( 0x1001 + selected_index ))
# Test mode initially maps 0x1002 to L1 1 and all other generated RNTIs to L1 0.
migration_target=1
if (( selected_rnti == 0x1002 )); then migration_target=0; fi

echo "Benchmark: ${ue_count} UEs, ${payload_size} bytes/UE, seed ${seed}" >&2
printf 'Migration: RNTI 0x%04x -> L1 %d\n' "$selected_rnti" "$migration_target" >&2

proxy_pid=""
vnf_pid=""
pnf0_pid=""
pnf1_pid=""

stop_process() {
    local pid="$1"
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}

cleanup() {
    stop_process "$vnf_pid"
    stop_process "$pnf0_pid"
    stop_process "$pnf1_pid"
    stop_process "$proxy_pid"
}
trap cleanup EXIT INT TERM

for binary in nfapi-proxy test_vnf_stub test_pnf_stub; do
    if [[ ! -x "${build_dir}/${binary}" ]]; then
        echo "Missing ${build_dir}/${binary}; configure with -DENABLE_MICROBENCHMARK=ON first" >&2
        exit 1
    fi
done

NFAPI_MICROBENCHMARK_LOG="$output" \
    THOR_CTRL_SOCK="$control_socket" \
    "${build_dir}/nfapi-proxy" --test-mode \
    --test-vnf-port 60101 --test-pnf-port 60110,60111 --p7-port 50112 \
    >/tmp/nfapi-proxy-microbenchmark.log 2>&1 &
proxy_pid=$!
sleep 1
if ! kill -0 "$proxy_pid" 2>/dev/null || [[ ! -S "$control_socket" ]]; then
    echo "Proxy failed to initialize; see /tmp/nfapi-proxy-microbenchmark.log" >&2
    exit 1
fi

"${build_dir}/test_pnf_stub" 60110 50112 0 "$iterations" "$payload_size" \
    >/tmp/pnf-stub-0-microbenchmark.log 2>&1 &
pnf0_pid=$!
"${build_dir}/test_pnf_stub" 60111 50112 1 "$iterations" "$payload_size" \
    >/tmp/pnf-stub-1-microbenchmark.log 2>&1 &
pnf1_pid=$!
"${build_dir}/test_vnf_stub" 60101 50112 "$iterations" "$payload_size" "$ue_count" \
    >/tmp/vnf-stub-microbenchmark.log 2>&1 &
vnf_pid=$!

migration_delay=$((duration / 2))
sleep "$migration_delay"
THOR_CTRL_SOCK="$control_socket" \
    "${build_dir}/control_client" migrate "$selected_rnti" "$migration_target" >&2
sleep $((duration - migration_delay))
stop_process "$vnf_pid"; vnf_pid=""
stop_process "$pnf0_pid"; pnf0_pid=""
stop_process "$pnf1_pid"; pnf1_pid=""
stop_process "$proxy_pid"; proxy_pid=""

if [[ ! -s "$output" ]]; then
    echo "Benchmark produced no events; see /tmp/nfapi-proxy-microbenchmark.log" >&2
    exit 1
fi

python3 "${script_dir}/analyze_latency.py" "$output" \
    --output-json "$result" \
    --payload-size "$payload_size" \
    --ues "$ue_count" \
    --seed "$seed" \
    --migrated-rnti "$(printf '0x%04x' "$selected_rnti")" \
    --migration-target "$migration_target"
echo "Raw events: $output" >&2
echo "DL/UL summary: $result" >&2
