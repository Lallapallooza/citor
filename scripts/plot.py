#!/usr/bin/env python3
# Renders the headline benchmark plots from a results.json produced by
# `scripts/run_bench.sh`. Outputs static SVGs under docs/bench/ -- GitHub
# Markdown renders these inline without executing JavaScript, which keeps
# the published charts diff-able under git and free of runtime dependencies
# on the reader's side.
#
# Plots produced (one SVG each):
#   <family>_geomean.svg        Per-family geomean speedup citor vs each
#                               competitor; horizontal bar, log x-axis,
#                               diagonal at 1.0.
#   <family>_scatter.svg        Per-workload scatter, citor on Y, competitor
#                               on X, log-log axes, identity line. Each
#                               workload is one dot; off-diagonal is visible
#                               at a glance.
#   where_citor_loses.svg       Sorted bar of cells where citor's median is
#                               worse than the best competitor by a margin.
#
# Usage:
#   scripts/plot.py path/to/results.json --out docs/bench
#
# Format note: parallel_bench's --export schema is not formally specified, so
# this script reads it defensively. The expected shape is
#   { "rows": [ {"workload": str, "pool": str, "median_ns": float,
#                "err_pct": float, "primitive_family": str }, ... ] }
# If your results lack `primitive_family`, the plotter derives it by stripping
# the workload's trailing `_j*` / `_n*` suffixes and taking the leading token.

from __future__ import annotations

import argparse
import collections
import json
import math
import statistics
import sys
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError as exc:
    sys.stderr.write(
        "matplotlib is required for plots: pip install matplotlib\n"
    )
    raise SystemExit(2) from exc


# Pinned style. Avoids seaborn / non-default rcParams so SVG output is
# byte-stable across matplotlib versions when the data does not change.
plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 10,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "savefig.bbox": "tight",
    "svg.fonttype": "none",
})


def derive_family(workload: str) -> str:
    head = workload.split("_")[0].lower()
    aliases = {
        "for": "parallelFor",
        "parallelfor": "parallelFor",
        "reduce": "parallelReduce",
        "parallelreduce": "parallelReduce",
        "scan": "parallelScan",
        "parallelscan": "parallelScan",
        "chain": "parallelChain",
        "parallelchain": "parallelChain",
        "runplex": "runPlex",
        "plex": "runPlex",
        "bulk": "bulkForQueries",
        "bulkforqueries": "bulkForQueries",
        "forkjoin": "forkJoin",
        "fj": "forkJoin",
        "submitdetached": "submitDetached",
        "detached": "submitDetached",
    }
    return aliases.get(head, head)


def load_rows(path: Path) -> list[dict]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    rows = raw.get("rows") or raw.get("benchmarks") or raw
    out = []
    for row in rows:
        wl = row.get("workload") or row.get("name")
        pool = row.get("pool") or row.get("competitor")
        median = row.get("median_ns") or row.get("real_time")
        if wl is None or pool is None or median is None:
            continue
        out.append({
            "workload": wl,
            "pool": pool,
            "median_ns": float(median),
            "err_pct": float(row.get("err_pct", 0.0)),
            "family": row.get("primitive_family") or derive_family(wl),
        })
    return out


def geomean(values: list[float]) -> float:
    if not values:
        return float("nan")
    return math.exp(statistics.fmean(math.log(v) for v in values if v > 0))


def per_workload_ratios(rows: list[dict]) -> dict:
    # For each (workload, competitor) pair, compute citor_median / competitor_median.
    # >1 means citor is slower (loses).
    by_wl = collections.defaultdict(dict)
    for row in rows:
        by_wl[row["workload"]][row["pool"]] = row
    ratios: dict = collections.defaultdict(dict)
    for wl, pools in by_wl.items():
        citor = pools.get("citor") or pools.get("citor::ThreadPool")
        if citor is None:
            continue
        for name, row in pools.items():
            if name == citor["pool"]:
                continue
            if row["median_ns"] <= 0:
                continue
            ratios[wl][name] = citor["median_ns"] / row["median_ns"]
    return ratios


def plot_family_geomeans(rows: list[dict], out_dir: Path) -> None:
    by_family = collections.defaultdict(list)
    ratios = per_workload_ratios(rows)
    rows_by_wl = {row["workload"]: row for row in rows}
    for wl, comp_ratios in ratios.items():
        family = rows_by_wl[wl]["family"]
        for comp, r in comp_ratios.items():
            if r > 0:
                by_family[(family, comp)].append(r)

    families = sorted({k[0] for k in by_family})
    competitors = sorted({k[1] for k in by_family})
    for family in families:
        labels = []
        speedups = []
        for comp in competitors:
            vs = by_family.get((family, comp), [])
            if not vs:
                continue
            gm = geomean(vs)
            if not math.isfinite(gm) or gm <= 0:
                continue
            labels.append(comp)
            speedups.append(1.0 / gm)
        if not speedups:
            continue
        order = sorted(range(len(labels)), key=lambda i: speedups[i])
        labels = [labels[i] for i in order]
        speedups = [speedups[i] for i in order]
        fig, ax = plt.subplots(figsize=(7, max(2.0, 0.4 * len(labels))))
        ax.barh(labels, speedups, color="#4682b4")
        ax.axvline(1.0, color="#888", linewidth=1, linestyle="--")
        ax.set_xscale("log")
        ax.set_xlabel(f"speedup of citor over competitor (geomean across {family} workloads)")
        ax.set_title(f"{family}: citor vs competitors")
        out = out_dir / f"{family}_geomean.svg"
        fig.savefig(out)
        plt.close(fig)
        print(f"plot: {out}")


def plot_scatter(rows: list[dict], out_dir: Path) -> None:
    by_family = collections.defaultdict(list)
    rows_by_wl = collections.defaultdict(dict)
    for row in rows:
        rows_by_wl[row["workload"]][row["pool"]] = row
    for wl, pools in rows_by_wl.items():
        citor = pools.get("citor")
        if citor is None:
            continue
        family = citor["family"]
        for comp, comp_row in pools.items():
            if comp == "citor":
                continue
            by_family[family].append({
                "workload": wl,
                "competitor": comp,
                "citor_ns": citor["median_ns"],
                "comp_ns": comp_row["median_ns"],
            })

    for family, points in by_family.items():
        if not points:
            continue
        fig, ax = plt.subplots(figsize=(6, 6))
        comps = sorted({p["competitor"] for p in points})
        cmap = plt.get_cmap("tab10")
        for idx, comp in enumerate(comps):
            xs = [p["comp_ns"] for p in points if p["competitor"] == comp]
            ys = [p["citor_ns"] for p in points if p["competitor"] == comp]
            ax.scatter(xs, ys, label=comp, color=cmap(idx % 10), s=18, alpha=0.7)
        all_ns = [p["comp_ns"] for p in points] + [p["citor_ns"] for p in points]
        if all_ns:
            lo, hi = min(all_ns) * 0.9, max(all_ns) * 1.1
            ax.plot([lo, hi], [lo, hi], color="#888", linewidth=1, linestyle="--")
            ax.set_xlim(lo, hi)
            ax.set_ylim(lo, hi)
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel("competitor median ns/op")
        ax.set_ylabel("citor median ns/op")
        ax.set_title(f"{family}: per-workload scatter (below diagonal = citor faster)")
        ax.legend(loc="best", fontsize=8, framealpha=0.6)
        out = out_dir / f"{family}_scatter.svg"
        fig.savefig(out)
        plt.close(fig)
        print(f"plot: {out}")


def plot_where_citor_loses(rows: list[dict], out_dir: Path) -> None:
    rows_by_wl = collections.defaultdict(dict)
    for row in rows:
        rows_by_wl[row["workload"]][row["pool"]] = row
    losses = []
    for wl, pools in rows_by_wl.items():
        citor = pools.get("citor")
        if citor is None or citor["median_ns"] <= 0:
            continue
        best_comp = None
        best_ns = float("inf")
        for comp, comp_row in pools.items():
            if comp == "citor":
                continue
            if 0 < comp_row["median_ns"] < best_ns:
                best_ns = comp_row["median_ns"]
                best_comp = comp
        if best_comp is None or best_ns >= citor["median_ns"]:
            continue
        losses.append({
            "workload": wl,
            "best_comp": best_comp,
            "ratio": citor["median_ns"] / best_ns,
        })
    if not losses:
        print("plot: where_citor_loses (no losses; skipping)")
        return
    losses.sort(key=lambda l: l["ratio"], reverse=True)
    losses = losses[:20]
    labels = [f"{l['workload']}  (vs {l['best_comp']})" for l in losses]
    ratios = [l["ratio"] for l in losses]
    fig, ax = plt.subplots(figsize=(8, max(2.0, 0.35 * len(labels))))
    ax.barh(labels[::-1], ratios[::-1], color="#c44e52")
    ax.axvline(1.0, color="#888", linewidth=1, linestyle="--")
    ax.set_xlabel("citor median / best competitor median (>1 means citor is slower)")
    ax.set_title("Workloads where citor loses to the best competitor")
    out = out_dir / "where_citor_loses.svg"
    fig.savefig(out)
    plt.close(fig)
    print(f"plot: {out}")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("results", help="path to bench results.json")
    p.add_argument("--out", default="docs/bench", help="output directory")
    args = p.parse_args()

    rows = load_rows(Path(args.results))
    if not rows:
        sys.stderr.write("plot: no rows parsed; check results schema\n")
        return 1
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    plot_family_geomeans(rows, out_dir)
    plot_scatter(rows, out_dir)
    plot_where_citor_loses(rows, out_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
