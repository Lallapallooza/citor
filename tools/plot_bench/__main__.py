"""CLI entry point: render SVG charts from a `parallel_bench --export` JSON."""

import argparse
import sys
from pathlib import Path

from matplotlib.figure import Figure

from tools.plot_bench.loader import derive_family, load_export
from tools.plot_bench.plot import (
    WorkloadFigureInputs,
    build_family_geomean_figure,
    build_family_scatter_figure,
    build_losses_figure,
    build_overview_figure,
    build_workload_figure,
)


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="python -m tools.plot_bench",
        description="Render SVG charts from a `parallel_bench --export` JSON.",
    )
    p.add_argument(
        "--input",
        "-i",
        type=Path,
        required=True,
        help="JSON file produced by `parallel_bench --export`.",
    )
    p.add_argument(
        "--output",
        "-o",
        type=Path,
        required=True,
        help="Output directory for SVG files. Created if missing.",
    )
    p.add_argument(
        "--baseline",
        type=str,
        default="citor::ThreadPool",
        help="Pool-name prefix used as the baseline for ratios. Default 'citor::ThreadPool'.",
    )
    p.add_argument(
        "--filter",
        action="append",
        default=[],
        metavar="SUBSTR",
        help="Only render workloads whose name contains SUBSTR. Repeatable; OR-ed.",
    )
    p.add_argument(
        "--no-per-workload",
        action="store_true",
        help="Skip the per-workload strip charts.",
    )
    p.add_argument(
        "--no-overview",
        action="store_true",
        help="Skip the cross-workload overview.",
    )
    p.add_argument(
        "--no-family",
        action="store_true",
        help="Skip the per-primitive-family geomean and scatter charts.",
    )
    p.add_argument(
        "--no-losses",
        action="store_true",
        help="Skip the 'where citor loses' summary.",
    )
    return p.parse_args(argv)


def _matches(name: str, filters: list[str]) -> bool:
    if not filters:
        return True
    return any(f in name for f in filters)


def _save(fig: Figure, path: Path) -> None:
    fig.savefig(path, format="svg", bbox_inches=None, pad_inches=0.0)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    in_path: Path = args.input
    out_dir: Path = args.output

    if not in_path.is_file():
        sys.stderr.write(f"plot_bench: input not found: {in_path}\n")
        return 1

    context, samples = load_export(in_path)
    if samples.empty:
        sys.stderr.write(f"plot_bench: {in_path} has no samples\n")
        return 1

    out_dir.mkdir(parents=True, exist_ok=True)

    workloads = sorted(samples["workload"].drop_duplicates().tolist())
    matched = [w for w in workloads if _matches(w, args.filter)]
    if not matched:
        sys.stderr.write("plot_bench: no workloads matched filter\n")
        return 1

    written: list[Path] = []

    if not args.no_per_workload:
        for workload in matched:
            sub = samples[samples["workload"] == workload]
            fig = build_workload_figure(
                WorkloadFigureInputs(
                    workload=workload,
                    samples=sub,
                    context=context,
                    baseline_pool_prefix=args.baseline,
                )
            )
            out_path = out_dir / f"{_safe_filename(workload)}.svg"
            _save(fig, out_path)
            written.append(out_path)

    filtered = samples[samples["workload"].isin(matched)]

    if not args.no_overview and len(matched) > 1:
        fig = build_overview_figure(filtered, context, baseline_pool_prefix=args.baseline)
        out_path = out_dir / "_overview.svg"
        _save(fig, out_path)
        written.append(out_path)

    if not args.no_family:
        families = sorted({derive_family(w) for w in matched})
        for family in families:
            fig = build_family_geomean_figure(filtered, context, family, citor_pool=args.baseline)
            out_path = out_dir / f"_family_{_safe_filename(family)}_geomean.svg"
            _save(fig, out_path)
            written.append(out_path)

            fig = build_family_scatter_figure(filtered, context, family, citor_pool=args.baseline)
            out_path = out_dir / f"_family_{_safe_filename(family)}_scatter.svg"
            _save(fig, out_path)
            written.append(out_path)

    if not args.no_losses:
        fig = build_losses_figure(filtered, context, citor_pool=args.baseline)
        out_path = out_dir / "_losses.svg"
        _save(fig, out_path)
        written.append(out_path)

    for p in written:
        print(p)
    return 0


def _safe_filename(name: str) -> str:
    return "".join(ch if ch.isalnum() or ch in ("-", "_", ".") else "_" for ch in name)


if __name__ == "__main__":
    raise SystemExit(main())
