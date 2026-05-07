#!/usr/bin/env bash
# User-facing bench runner. Wraps `parallel_bench` with the host invariants the
# fairness audit expects: isolated CPUs, performance governor, no turbo
# throttle from a thermal-runaway sibling, ASLR neutralised. Stamps a
# host.json header so two runs from different machines are comparable.
#
# Usage:
#   scripts/run_bench.sh                       # full sweep, ~2-3h on a 9950X3D
#   scripts/run_bench.sh --filter parallel_for # one workload family
#   scripts/run_bench.sh --out bench_out/$(hostname)/$(git rev-parse HEAD)
#
# Reads (does not modify): governor / turbo / SMT / ASLR. Aborts if another
# parallel_bench is already running on the host (the bench-poisoning rule).

set -euo pipefail

BENCH=${BENCH:-./build/benchmark/parallel_bench}
TASKSET=${TASKSET:-0-15}
FILTER=""
OUT_DIR=""
EXTRA_ARGS=()

usage() {
    cat <<'EOF'
Usage: scripts/run_bench.sh [options]

Options:
  --filter PATTERN   Workload-name filter passed to parallel_bench
  --out DIR          Output directory (default: bench_out/<host>/<short-sha>)
  --bench PATH       Override path to parallel_bench
  --taskset MASK     CPU mask for taskset (default: 0-15)
  --                 Forward remaining args to parallel_bench
  -h, --help         This help.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --filter)  FILTER="$2"; shift 2 ;;
        --out)     OUT_DIR="$2"; shift 2 ;;
        --bench)   BENCH="$2"; shift 2 ;;
        --taskset) TASKSET="$2"; shift 2 ;;
        --)        shift; EXTRA_ARGS+=("$@"); break ;;
        -h|--help) usage; exit 0 ;;
        *)         echo "unknown option: $1" >&2; usage; exit 2 ;;
    esac
done

if [[ ! -x "$BENCH" ]]; then
    echo "error: $BENCH not built. Configure with -DCITOR_BUILD_BENCHMARK=ON and build the parallel_bench target." >&2
    exit 2
fi

if pgrep -x parallel_bench >/dev/null 2>&1; then
    echo "error: another parallel_bench is already running. Wait for it to finish (the bench-poisoning rule)." >&2
    exit 2
fi

if [[ -z "$OUT_DIR" ]]; then
    host=$(hostname -s)
    sha=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
    OUT_DIR="bench_out/${host}/${sha}"
fi
mkdir -p "$OUT_DIR"

# Capture host disclosure once per run. Fields chosen to match what
# docs/BENCHMARKS.md asks readers to disclose.
{
    echo "{"
    echo "  \"timestamp_iso8601\": \"$(date -u +%FT%TZ)\","
    echo "  \"hostname\": \"$(hostname -s)\","
    echo "  \"kernel\": \"$(uname -r)\","
    echo "  \"distro\": \"$(. /etc/os-release; echo "${PRETTY_NAME:-unknown}")\","
    echo "  \"cpu\": \"$(grep -m1 '^model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')\","
    echo "  \"cpu_cores_logical\": $(nproc),"
    echo "  \"governor\": \"$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)\","
    echo "  \"turbo_no_turbo\": \"$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo unknown)\","
    echo "  \"smt_active\": \"$(cat /sys/devices/system/cpu/smt/active 2>/dev/null || echo unknown)\","
    echo "  \"aslr\": \"$(cat /proc/sys/kernel/randomize_va_space 2>/dev/null || echo unknown)\","
    echo "  \"taskset_mask\": \"$TASKSET\","
    echo "  \"git_sha\": \"$(git rev-parse HEAD 2>/dev/null || echo unknown)\","
    echo "  \"bench_binary\": \"$(readlink -f "$BENCH")\","
    echo "  \"compiler\": \"$("$BENCH" --version 2>/dev/null | head -1 || echo unknown)\""
    echo "}"
} > "$OUT_DIR/host.json"

cmd=(taskset -c "$TASKSET" "$BENCH")
if [[ -n "$FILTER" ]]; then
    cmd+=(--filter "$FILTER")
fi
cmd+=(--export "$OUT_DIR/results.json")
cmd+=("${EXTRA_ARGS[@]}")

echo "host.json: $OUT_DIR/host.json"
echo "running:   ${cmd[*]}"
echo "log:       $OUT_DIR/run.log"

start=$(date +%s)
"${cmd[@]}" 2>&1 | tee "$OUT_DIR/run.log"
rc=${PIPESTATUS[0]}
elapsed=$(( $(date +%s) - start ))

echo "elapsed:   ${elapsed}s"
echo "exit:      $rc"
exit "$rc"
