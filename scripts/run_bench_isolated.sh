#!/usr/bin/env bash
# Run each workload in its own process so a competitor pool's segfault in one
# cell doesn't kill the entire bench run.
set -u
BENCH=./build/benchmark/parallel_bench
OUT=bench_out/per_workload
LIST=bench_out/workloads.txt
LOG=bench_out/run.log
: > "$LOG"
ok=0; bad=0
while IFS= read -r wl; do
  [[ -z "$wl" ]] && continue
  json="${OUT}/${wl}.json"
  txt="${OUT}/${wl}.txt"
  rm -f "$json"
  start=$(date +%s)
  taskset -c 0-15 timeout 1800 "$BENCH" \
    --filter "$wl" \
    --with-tail-percentiles \
    --export "$json" \
    > "$txt" 2>&1
  rc=$?
  dur=$(( $(date +%s) - start ))
  size=$(stat -c%s "$json" 2>/dev/null || echo 0)
  if [[ $rc -eq 0 && $size -gt 0 ]]; then
    echo "[OK ${dur}s ${size}B] $wl" | tee -a "$LOG"
    ok=$((ok+1))
  else
    echo "[FAIL rc=$rc dur=${dur}s] $wl" | tee -a "$LOG"
    bad=$((bad+1))
  fi
done < "$LIST"
echo "===== done: $ok ok, $bad failed =====" | tee -a "$LOG"
