"""Compare two bench JSON exports cell-by-cell. Emits a delta% table."""

import argparse
import json
import statistics
from pathlib import Path


def _load(path: Path) -> dict[tuple[str, str], float]:
    samples: dict[tuple[str, str], list[float]] = {}
    paths = sorted(path.glob("*.json")) if path.is_dir() else [path]
    for p in paths:
        try:
            doc = json.loads(p.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        for s in doc.get("samples", []):
            key = (s["workload"], s["pool"])
            samples.setdefault(key, []).append(s["ns"])
    return {k: statistics.median(v) for k, v in samples.items() if v}


def add_parser(sub: argparse._SubParsersAction) -> None:
    p = sub.add_parser("diff", help="diff two bench JSON exports")
    p.add_argument("baseline", help="JSON file or per-workload dir")
    p.add_argument("target", help="JSON file or per-workload dir")
    p.add_argument("--filter", default="", help="substring filter on workload name")
    p.add_argument("--engine", default="citor", help="substring filter on pool name")
    p.add_argument(
        "--threshold",
        type=float,
        default=0.02,
        help="flag |delta| above this (default 0.02 = 2 percent)",
    )
    p.set_defaults(func=run)


def run(args: argparse.Namespace) -> int:
    base = _load(Path(args.baseline))
    targ = _load(Path(args.target))

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

    threshold_pct = args.threshold * 100
    print(f"{'delta%':>8}  {'baseline_ns':>14}  {'target_ns':>14}  {'ratio':>7}  workload | pool")
    print(f"{'-' * 8}  {'-' * 14}  {'-' * 14}  {'-' * 7}  -----------")
    wins = regressions = 0
    for delta_pct, wl, pool, b, t, ratio in rows:
        flag = ""
        if delta_pct > threshold_pct:
            flag = " REGRESS"
            regressions += 1
        elif delta_pct < -threshold_pct:
            flag = " WIN"
            wins += 1
        print(f"{delta_pct:>+7.2f}%  {b:>14.0f}  {t:>14.0f}  {ratio:>7.3f}  {wl} | {pool}{flag}")

    print()
    print(f"Total cells compared: {len(rows)}")
    print(f"Wins (<= {-threshold_pct:.0f}%): {wins}")
    print(f"Regressions (> {threshold_pct:.0f}%): {regressions}")
    return 0
