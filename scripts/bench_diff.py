#!/usr/bin/env python3
"""Compare per-cell median ns across two bench-export JSONs.

Reads either a single bench-full.json or a directory of per-workload JSONs
(produced by scripts/run_bench_isolated.sh) and emits a ratio table per
(workload, pool) row.

Usage:
    bench_diff.py BASELINE TARGET [--filter PATTERN] [--engine SUBSTR]

BASELINE / TARGET may be either a JSON file or a directory of JSONs.
"""

import argparse
import json
import os
import statistics
import sys
from pathlib import Path

def load(p):
    """Return {(workload, pool): median_ns}."""
    samples_by_key = {}
    p = Path(p)
    if p.is_dir():
        for json_file in sorted(p.glob("*.json")):
            try:
                with open(json_file) as f:
                    data = json.load(f)
            except (OSError, json.JSONDecodeError):
                continue
            for s in data.get("samples", []):
                key = (s["workload"], s["pool"])
                samples_by_key.setdefault(key, []).append(s["ns"])
    else:
        with open(p) as f:
            data = json.load(f)
        for s in data.get("samples", []):
            key = (s["workload"], s["pool"])
            samples_by_key.setdefault(key, []).append(s["ns"])

    return {k: statistics.median(v) for k, v in samples_by_key.items() if v}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("baseline")
    ap.add_argument("target")
    ap.add_argument("--filter", default="", help="substring match on workload name")
    ap.add_argument("--engine", default="citor", help="substring match on pool name (default citor)")
    ap.add_argument("--threshold", type=float, default=0.02,
                    help="highlight cells where |delta| > threshold (default 0.02 = 2%%)")
    args = ap.parse_args()

    base = load(args.baseline)
    targ = load(args.target)

    rows = []
    for key in sorted(set(base) & set(targ)):
        wl, pool = key
        if args.filter and args.filter not in wl:
            continue
        if args.engine and args.engine not in pool:
            continue
        b = base[key]
        t = targ[key]
        if b <= 0:
            continue
        ratio = t / b
        delta_pct = (ratio - 1.0) * 100.0
        rows.append((delta_pct, wl, pool, b, t, ratio))

    rows.sort()

    print(f"{'Δ%':>8}  {'baseline_ns':>14}  {'target_ns':>14}  {'ratio':>7}  workload | pool")
    print(f"{'-'*8}  {'-'*14}  {'-'*14}  {'-'*7}  -----------")
    regressions = 0
    wins = 0
    threshold_pct = args.threshold * 100
    for delta_pct, wl, pool, b, t, ratio in rows:
        flag = ""
        if delta_pct > threshold_pct:
            flag = " ⚠ REGRESS"
            regressions += 1
        elif delta_pct < -threshold_pct:
            flag = " ✓ WIN"
            wins += 1
        print(f"{delta_pct:>+7.2f}%  {b:>14.0f}  {t:>14.0f}  {ratio:>7.3f}  {wl} | {pool}{flag}")

    print()
    print(f"Total cells compared: {len(rows)}")
    print(f"Wins (≤{-threshold_pct:.0f}%): {wins}")
    print(f"Regressions (>{threshold_pct:.0f}%): {regressions}")

if __name__ == "__main__":
    sys.exit(main())
