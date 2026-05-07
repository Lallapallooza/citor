#!/usr/bin/env bash
# Smoke bench: a handful of representative workloads, ~90s wall-clock,
# enough signal to spot a 2-3x dispatch-path catastrophe but NOT enough to
# rank competitor pools. For full numbers, see scripts/run_bench.sh and
# docs/BENCHMARKS.md.
#
# Usage:
#   scripts/quickbench.sh
#   BENCH=path/to/parallel_bench scripts/quickbench.sh

set -euo pipefail

BENCH=${BENCH:-./build/benchmark/parallel_bench}
TASKSET=${TASKSET:-0-15}

# Five cells spanning the four primitive families that matter most. Names
# match the workload registrations in benchmark/*.cpp; if these go out of
# sync, the per-call timing surfaces it as a "no matching workload" error
# from parallel_bench.
WORKLOADS=(
    "parallel_for_n1M_j16"
    "parallel_reduce_n1M_j16"
    "forkjoin_fib_n30_j16"
    "parallel_chain_pipeline_j16"
    "runplex_stencil_j16"
)

if [[ ! -x "$BENCH" ]]; then
    echo "error: $BENCH not built. Configure with -DCITOR_BUILD_BENCHMARK=ON and build the parallel_bench target." >&2
    exit 2
fi

if pgrep -x parallel_bench >/dev/null 2>&1; then
    echo "error: another parallel_bench is already running. Wait for it to finish." >&2
    exit 2
fi

filter=$(IFS='|'; echo "${WORKLOADS[*]}")

echo "quickbench: $filter"
echo "host:       $(hostname -s) / $(grep -m1 '^model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
echo "governor:   $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
echo

start=$(date +%s)
taskset -c "$TASKSET" "$BENCH" --filter "$filter"
elapsed=$(( $(date +%s) - start ))
echo
echo "elapsed: ${elapsed}s"
