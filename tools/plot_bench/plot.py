"""Figure builders for the plot_bench package.

The package emits five chart kinds, all SVG, all using the matplotlib OO API:

- per-workload strip chart (`build_workload_figure`): one row per pool, log-x
  time axis, faded raw dots + bootstrap CI bar + median diamond.
- cross-workload overview (`build_overview_figure`): one row per workload,
  pools as dots laid out by their median-ratio against the citor baseline.
- per-family geomean bars (`build_family_geomean_figure`): geomean speedup of
  citor over each competitor across all workloads in one primitive family.
- per-family scatter (`build_family_scatter_figure`): citor median ns/op on Y,
  competitor median ns/op on X, log-log axes with an identity diagonal.
- losses chart (`build_losses_figure`): horizontal bar of the workloads where
  citor's median is worse than the best competitor, sorted descending.
"""

import collections
import math
import statistics
from dataclasses import dataclass
from typing import Any

import numpy as np
import pandas as pd
from matplotlib.backends.backend_svg import FigureCanvasSVG
from matplotlib.figure import Figure
from matplotlib.lines import Line2D
from matplotlib.ticker import FixedLocator, FuncFormatter, NullLocator

from tools.plot_bench import style
from tools.plot_bench.loader import Context, bootstrap_median_ci, derive_family


@dataclass(frozen=True, slots=True)
class WorkloadFigureInputs:
    workload: str
    samples: pd.DataFrame
    context: Context
    baseline_pool_prefix: str = "citor::ThreadPool"


_BASELINE_LINE_COLOR = "#0072B2"


def build_workload_figure(inputs: WorkloadFigureInputs) -> Figure:
    """Per-workload strip chart: one row per pool, log time axis."""
    df = inputs.samples
    if df.empty:
        return _build_empty_figure(inputs.workload, "no samples")

    pools = list(df["pool"].drop_duplicates())
    medians: dict[str, float] = {}
    cis: dict[str, tuple[float, float, float]] = {}
    for pool in pools:
        ns = df.loc[df["pool"] == pool, "ns"].to_numpy(dtype="float64", copy=False)
        cis[pool] = bootstrap_median_ci(ns)
        medians[pool] = cis[pool][0]
    pools_sorted = sorted(pools, key=lambda p: medians.get(p, float("inf")))

    baseline_pool = next(
        (p for p in pools_sorted if p.startswith(inputs.baseline_pool_prefix)),
        None,
    )
    baseline_median = medians.get(baseline_pool, float("nan")) if baseline_pool else float("nan")

    n_pools = len(pools_sorted)
    fig_height = max(3.6, 0.55 * n_pools + 2.4)
    fig = _new_svg_figure(figsize=(10.0, fig_height))

    gs = fig.add_gridspec(
        nrows=1,
        ncols=1,
        left=0.30,
        right=0.86,
        top=0.80,
        bottom=0.18,
    )
    ax = fig.add_subplot(gs[0, 0])

    _style_axes(ax, pools_sorted, df["ns"].to_numpy())
    ax.invert_yaxis()

    rng = np.random.default_rng(0xC17071A)
    for y_idx, pool in enumerate(pools_sorted):
        ns = df.loc[df["pool"] == pool, "ns"].to_numpy(dtype="float64", copy=False)
        color = style.pool_color(pool)
        jitter = rng.uniform(-0.18, 0.18, size=ns.size)
        ax.scatter(
            ns,
            np.full_like(ns, y_idx, dtype="float64") + jitter,
            s=18.0,
            color=color,
            alpha=0.30,
            edgecolors="none",
            zorder=2,
        )
        median, lo, hi = cis[pool]
        if np.isfinite(lo) and np.isfinite(hi):
            ax.plot(
                [lo, hi],
                [y_idx, y_idx],
                color=color,
                linewidth=2.4,
                solid_capstyle="round",
                alpha=0.85,
                zorder=3,
            )
        ax.plot(
            median,
            y_idx,
            marker="D",
            markersize=8.5,
            markerfacecolor=color,
            markeredgecolor="white",
            markeredgewidth=1.4,
            zorder=4,
        )

    if baseline_pool is not None and np.isfinite(baseline_median) and baseline_median > 0:
        ax.axvline(
            baseline_median,
            color=_BASELINE_LINE_COLOR,
            linewidth=1.2,
            linestyle="--",
            alpha=0.55,
            zorder=1,
        )
        ax.text(
            baseline_median,
            -0.5,
            f"  {baseline_pool} median",
            color=_BASELINE_LINE_COLOR,
            fontsize=8.5,
            fontstyle="italic",
            ha="left",
            va="bottom",
            zorder=5,
        )

        for y_idx, pool in enumerate(pools_sorted):
            median = medians[pool]
            if not np.isfinite(median) or median <= 0:
                continue
            ratio = median / baseline_median
            label = _format_ratio(ratio)
            color = style.MUTED if pool == baseline_pool else style.TEXT
            ax.annotate(
                label,
                xy=(1.005, y_idx),
                xycoords=("axes fraction", "data"),
                xytext=(0, 0),
                textcoords="offset points",
                fontsize=9.0,
                color=color,
                ha="left",
                va="center",
                zorder=4,
            )

    _draw_header(fig, inputs.workload)
    _draw_subtitle(fig, inputs.context)
    _draw_header_rule(fig)
    _draw_caption(fig, inputs.context)
    _draw_legend(fig, baseline_pool)

    return fig


def build_overview_figure(
    samples: pd.DataFrame,
    context: Context,
    *,
    baseline_pool_prefix: str = "citor::ThreadPool",
) -> Figure:
    """Cross-workload overview: per workload, pool dots laid out by median-ratio."""
    if samples.empty:
        return _build_empty_figure("citor benchmark overview", "no samples")

    workloads = sorted(samples["workload"].drop_duplicates().tolist())
    rows: list[tuple[str, str, float, float]] = []
    for workload in workloads:
        per_workload = samples[samples["workload"] == workload]
        pools = list(per_workload["pool"].drop_duplicates())
        baseline = None
        baseline_median = float("nan")
        for p in sorted(pools):
            if p.startswith(baseline_pool_prefix):
                baseline = p
                ns = per_workload.loc[per_workload["pool"] == p, "ns"].to_numpy("float64")
                baseline_median = float(np.median(ns)) if ns.size else float("nan")
                break
        if baseline is None or not np.isfinite(baseline_median) or baseline_median <= 0:
            continue
        for pool in pools:
            ns = per_workload.loc[per_workload["pool"] == pool, "ns"].to_numpy("float64")
            if ns.size == 0:
                continue
            median = float(np.median(ns))
            if not np.isfinite(median) or median <= 0:
                continue
            rows.append((workload, pool, median / baseline_median, median))

    if not rows:
        return _build_empty_figure("citor benchmark overview", "no baseline matches")

    workload_index = {wl: i for i, wl in enumerate(workloads)}

    n_rows = len(workloads)
    fig_height = max(5.0, 0.55 * n_rows + 3.2)
    fig = _new_svg_figure(figsize=(11.0, fig_height))
    top_frac = 1.0 - (1.4 / fig_height)
    bottom_frac = 1.0 / fig_height
    gs = fig.add_gridspec(
        nrows=1,
        ncols=1,
        left=0.27,
        right=0.97,
        top=top_frac,
        bottom=bottom_frac,
    )
    ax = fig.add_subplot(gs[0, 0])

    _style_axes_overview(ax, workloads)
    ax.invert_yaxis()

    for workload, pool, ratio, _ns in rows:
        y = workload_index[workload]
        color = style.pool_color(pool)
        ax.plot(
            ratio,
            y,
            marker="o",
            markersize=8.0,
            markerfacecolor=color,
            markeredgecolor="white",
            markeredgewidth=1.0,
            alpha=0.92,
            zorder=3,
        )

    ax.axvline(
        1.0,
        color=_BASELINE_LINE_COLOR,
        linewidth=1.3,
        linestyle="--",
        alpha=0.6,
        zorder=1,
    )
    ax.text(
        1.0,
        -0.5,
        "  parity (ratio = 1.0)",
        color=_BASELINE_LINE_COLOR,
        fontsize=8.5,
        fontstyle="italic",
        ha="left",
        va="bottom",
        zorder=5,
    )

    _draw_header(fig, "citor benchmark overview")
    _draw_subtitle(fig, context)
    _draw_header_rule(fig)
    _draw_caption(fig, context)

    return fig


def build_family_geomean_figure(
    samples: pd.DataFrame,
    context: Context,
    family: str,
    *,
    citor_pool: str = "citor::ThreadPool",
) -> Figure:
    """Geomean speedup of citor over each competitor across all workloads in `family`."""
    medians_by_workload = _medians_by_workload(samples)
    speedups: dict[str, list[float]] = collections.defaultdict(list)
    for workload, by_pool in medians_by_workload.items():
        if derive_family(workload) != family:
            continue
        citor_ns = _baseline_median(by_pool, citor_pool)
        if citor_ns is None:
            continue
        for pool, ns in by_pool.items():
            if pool == citor_pool or ns <= 0:
                continue
            speedups[pool].append(ns / citor_ns)

    rows: list[tuple[str, float]] = []
    for pool, ratios in speedups.items():
        gm = _geomean(ratios)
        if math.isfinite(gm) and gm > 0:
            rows.append((pool, gm))
    if not rows:
        return _build_empty_figure(f"{family}: citor vs competitors", "no overlapping workloads")

    rows.sort(key=lambda r: r[1])
    labels = [r[0] for r in rows]
    speeds = [r[1] for r in rows]

    fig_height = max(2.6, 0.45 * len(labels) + 2.4)
    fig = _new_svg_figure(figsize=(9.0, fig_height))
    gs = fig.add_gridspec(nrows=1, ncols=1, left=0.30, right=0.95, top=0.80, bottom=0.20)
    ax = fig.add_subplot(gs[0, 0])
    colors = [style.pool_color(pool) for pool in labels]
    ax.barh(labels, speeds, color=colors, edgecolor="white", linewidth=0.8)
    ax.axvline(1.0, color=_BASELINE_LINE_COLOR, linewidth=1.2, linestyle="--", alpha=0.6)
    ax.set_xscale("log")
    ax.set_xlabel(
        f"geomean speedup of citor over competitor ({family} workloads)",
        fontsize=11.0,
        color=style.TEXT,
        labelpad=8,
    )
    _style_minimal_axes(ax)

    _draw_header(fig, f"{family}: citor vs competitors")
    _draw_subtitle(fig, context)
    _draw_header_rule(fig)
    _draw_caption(fig, context)
    return fig


def build_family_scatter_figure(
    samples: pd.DataFrame,
    context: Context,
    family: str,
    *,
    citor_pool: str = "citor::ThreadPool",
) -> Figure:
    """Per-workload scatter for one primitive family. Each dot is a workload."""
    medians_by_workload = _medians_by_workload(samples)
    points: list[tuple[str, str, float, float]] = []
    for workload, by_pool in medians_by_workload.items():
        if derive_family(workload) != family:
            continue
        citor_ns = _baseline_median(by_pool, citor_pool)
        if citor_ns is None:
            continue
        for pool, ns in by_pool.items():
            if pool == citor_pool or ns <= 0:
                continue
            points.append((workload, pool, ns, citor_ns))

    if not points:
        return _build_empty_figure(f"{family}: per-workload scatter", "no overlapping workloads")

    fig = _new_svg_figure(figsize=(7.5, 6.5))
    gs = fig.add_gridspec(nrows=1, ncols=1, left=0.13, right=0.97, top=0.82, bottom=0.13)
    ax = fig.add_subplot(gs[0, 0])

    competitors = sorted({p[1] for p in points})
    for comp in competitors:
        xs = [p[2] for p in points if p[1] == comp]
        ys = [p[3] for p in points if p[1] == comp]
        ax.scatter(
            xs,
            ys,
            label=comp,
            color=style.pool_color(comp),
            s=22.0,
            alpha=0.78,
            edgecolors="white",
            linewidths=0.6,
            zorder=3,
        )

    all_ns = [p[2] for p in points] + [p[3] for p in points]
    lo, hi = min(all_ns) * 0.9, max(all_ns) * 1.1
    ax.plot(
        [lo, hi],
        [lo, hi],
        color=_BASELINE_LINE_COLOR,
        linewidth=1.0,
        linestyle="--",
        alpha=0.6,
    )
    ax.set_xlim(lo, hi)
    ax.set_ylim(lo, hi)
    ax.set_xscale("log")
    ax.set_yscale("log")

    ticks = style.pick_log_ticks(style.TIME_TICK_LATTICE_NS, lo, hi)
    ax.xaxis.set_major_locator(FixedLocator(ticks))
    ax.xaxis.set_major_formatter(FuncFormatter(style.fmt_time_ns))
    ax.yaxis.set_major_locator(FixedLocator(ticks))
    ax.yaxis.set_major_formatter(FuncFormatter(style.fmt_time_ns))
    ax.xaxis.set_minor_locator(NullLocator())
    ax.yaxis.set_minor_locator(NullLocator())

    ax.set_xlabel("competitor median ns/op", fontsize=11.0, color=style.TEXT, labelpad=8)
    ax.set_ylabel("citor median ns/op", fontsize=11.0, color=style.TEXT, labelpad=8)
    _style_minimal_axes(ax, draw_grid=True)

    legend = ax.legend(
        loc="upper left",
        fontsize=8.5,
        frameon=False,
        handlelength=1.6,
        handletextpad=0.5,
        labelspacing=0.4,
    )
    for txt in legend.get_texts():
        txt.set_color(style.TEXT)

    _draw_header(fig, f"{family}: per-workload scatter")
    _draw_subtitle(fig, context, suffix="below diagonal = citor faster")
    _draw_header_rule(fig)
    _draw_caption(fig, context)
    return fig


def build_losses_figure(
    samples: pd.DataFrame,
    context: Context,
    *,
    citor_pool: str = "citor::ThreadPool",
    top_n: int = 20,
) -> Figure:
    """Workloads where citor's median is worse than the best competitor."""
    medians_by_workload = _medians_by_workload(samples)
    losses: list[tuple[str, str, float]] = []
    for workload, by_pool in medians_by_workload.items():
        citor_ns = _baseline_median(by_pool, citor_pool)
        if citor_ns is None:
            continue
        best_pool = None
        best_ns = float("inf")
        for pool, ns in by_pool.items():
            if pool == citor_pool or ns <= 0:
                continue
            if ns < best_ns:
                best_ns = ns
                best_pool = pool
        if best_pool is None or best_ns >= citor_ns:
            continue
        losses.append((workload, best_pool, citor_ns / best_ns))

    if not losses:
        return _build_empty_figure(
            "Workloads where citor loses",
            "citor matches or beats every competitor on every workload",
        )

    losses.sort(key=lambda row: row[2], reverse=True)
    losses = losses[:top_n]
    labels = [f"{wl}  (vs {comp})" for wl, comp, _ in losses][::-1]
    ratios = [r for _, _, r in losses][::-1]

    fig_height = max(3.0, 0.42 * len(labels) + 2.4)
    fig = _new_svg_figure(figsize=(10.0, fig_height))
    gs = fig.add_gridspec(nrows=1, ncols=1, left=0.42, right=0.97, top=0.82, bottom=0.16)
    ax = fig.add_subplot(gs[0, 0])
    ax.barh(labels, ratios, color=style.BAND_LOSS, edgecolor="white", linewidth=0.7)
    ax.axvline(1.0, color=_BASELINE_LINE_COLOR, linewidth=1.2, linestyle="--", alpha=0.6)
    ax.set_xlabel(
        "citor median / best competitor median",
        fontsize=11.0,
        color=style.TEXT,
        labelpad=8,
    )
    _style_minimal_axes(ax, draw_grid=True)
    _draw_header(fig, "Workloads where citor loses")
    _draw_subtitle(fig, context)
    _draw_header_rule(fig)
    _draw_caption(fig, context)
    return fig


def _medians_by_workload(samples: pd.DataFrame) -> dict[str, dict[str, float]]:
    out: dict[str, dict[str, float]] = collections.defaultdict(dict)
    if samples.empty:
        return out
    grouped = samples.groupby(["workload", "pool"], observed=True)["ns"].median()
    for key, median in grouped.items():
        out[str(key[0])][str(key[1])] = float(median)  # type: ignore[index]
    return out


def _baseline_median(by_pool: dict[str, float], baseline: str) -> float | None:
    ns = by_pool.get(baseline)
    if ns is None or ns <= 0 or not math.isfinite(ns):
        return None
    return ns


def _geomean(values: list[float]) -> float:
    positive = [v for v in values if v > 0 and math.isfinite(v)]
    if not positive:
        return float("nan")
    return math.exp(statistics.fmean(math.log(v) for v in positive))


def _new_svg_figure(*, figsize: tuple[float, float]) -> Figure:
    fig = Figure(figsize=figsize, facecolor="white")
    FigureCanvasSVG(fig)
    return fig


def _style_axes(ax: Any, pools: list[str], ns_values: np.ndarray) -> None:
    ax.set_xscale("log")
    ax.set_yticks(list(range(len(pools))))
    ax.set_yticklabels(pools, fontsize=10.0, color=style.TEXT)
    ax.set_ylim(-0.6, len(pools) - 0.4)

    finite = ns_values[(ns_values > 0) & np.isfinite(ns_values)]
    if finite.size > 0:
        lo = float(finite.min()) / 1.20
        hi = float(finite.max()) * 1.20
    else:
        lo, hi = 1.0, 10.0
    ax.set_xlim(lo, hi)
    ticks = style.pick_log_ticks(style.TIME_TICK_LATTICE_NS, lo, hi)
    ax.xaxis.set_major_locator(FixedLocator(ticks))
    ax.xaxis.set_major_formatter(FuncFormatter(style.fmt_time_ns))
    ax.xaxis.set_minor_locator(NullLocator())

    _style_minimal_axes(ax, draw_grid=True)
    ax.set_xlabel("time (log)", fontsize=11.0, color=style.TEXT, labelpad=8)


def _style_axes_overview(ax: Any, workloads: list[str]) -> None:
    ax.set_xscale("log")
    ax.set_yticks(list(range(len(workloads))))
    ax.set_yticklabels(workloads, fontsize=9.5, color=style.TEXT)
    ax.set_ylim(-0.6, len(workloads) - 0.4)

    ax.xaxis.set_minor_locator(NullLocator())
    ax.xaxis.set_major_locator(FixedLocator([0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 25.0, 100.0]))
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:g}x"))
    ax.set_xlabel(
        "median runtime ratio vs citor::ThreadPool baseline",
        fontsize=11.0,
        color=style.TEXT,
        labelpad=8,
    )
    _style_minimal_axes(ax, draw_grid=True)


def _style_minimal_axes(ax: Any, *, draw_grid: bool = True) -> None:
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(style.AXIS)
        ax.spines[side].set_linewidth(0.7)
    ax.set_facecolor("white")
    if draw_grid:
        ax.grid(
            which="major",
            axis="x",
            color=style.GRID,
            linestyle="-",
            linewidth=0.6,
            alpha=1.0,
            zorder=0,
        )
    ax.set_axisbelow(True)
    ax.tick_params(
        axis="both",
        which="major",
        color=style.AXIS,
        labelcolor=style.TEXT,
        labelsize=10.0,
        length=3.5,
        width=0.8,
        pad=4,
    )
    ax.tick_params(axis="both", which="minor", length=0)


def _draw_header(fig: Figure, title: str) -> None:
    fig.text(
        0.04,
        0.945,
        title,
        fontsize=18.0,
        fontweight="bold",
        color=style.TEXT,
        ha="left",
        va="top",
    )
    fig.text(
        0.99,
        0.945,
        "lower is better",
        fontsize=11.0,
        fontweight="medium",
        color=style.MUTED,
        ha="right",
        va="top",
    )


def _draw_subtitle(fig: Figure, ctx: Context, *, suffix: str = "") -> None:
    parts: list[str] = []
    if ctx.citor_version:
        v = ctx.citor_version + (" (dirty)" if ctx.citor_dirty else "")
        parts.append(f"citor {v}")
    if ctx.cpu_model:
        parts.append(ctx.cpu_model)
    if ctx.taskset_cpus:
        parts.append(f"taskset {ctx.taskset_cpus}")
    if suffix:
        parts.append(suffix)
    if parts:
        fig.text(
            0.04,
            0.905,
            "    |    ".join(parts),
            fontsize=10.5,
            color=style.MUTED,
            ha="left",
            va="top",
        )


def _draw_header_rule(fig: Figure) -> None:
    fig.add_artist(
        Line2D(
            [0.04, 0.99],
            [0.86, 0.86],
            transform=fig.transFigure,
            color=style.AXIS,
            linewidth=0.9,
            alpha=0.25,
            solid_capstyle="butt",
        )
    )


def _draw_caption(fig: Figure, ctx: Context) -> None:
    bits: list[str] = []
    if ctx.datetime_utc:
        bits.append(ctx.datetime_utc)
    if ctx.hostname:
        bits.append(ctx.hostname)
    if ctx.kernel:
        bits.append(ctx.kernel)
    if ctx.compiler:
        bits.append(f"{ctx.compiler} {ctx.compiler_version}".strip())
    fig.text(
        0.5,
        0.045,
        "    |    ".join(bits) if bits else "",
        fontsize=8.5,
        color=style.MUTED,
        ha="center",
        va="bottom",
    )
    if ctx.checklist:
        gates = ", ".join(f"{g.name}={g.status}" for g in ctx.checklist if g.status != "PASS")
        if gates:
            fig.text(
                0.5,
                0.022,
                f"WARN  {gates}",
                fontsize=8.5,
                color=style.BAND_LOSS,
                ha="center",
                va="bottom",
            )


def _draw_legend(fig: Figure, baseline_pool: str | None) -> None:
    handles: list[Line2D] = [
        Line2D(
            [0],
            [0],
            marker="D",
            linestyle="none",
            markersize=8.5,
            markeredgecolor="white",
            markeredgewidth=1.4,
            color=style.AXIS,
            label="median",
        ),
        Line2D(
            [0],
            [0],
            color=style.AXIS,
            linewidth=2.4,
            label="95% bootstrap CI",
        ),
        Line2D(
            [0],
            [0],
            marker="o",
            linestyle="none",
            markersize=6.0,
            markeredgecolor="none",
            color=style.AXIS,
            alpha=0.30,
            label="raw sample",
        ),
    ]
    if baseline_pool is not None:
        handles.append(
            Line2D(
                [0],
                [0],
                color=_BASELINE_LINE_COLOR,
                linestyle="--",
                linewidth=1.2,
                alpha=0.7,
                label="baseline median",
            )
        )
    legend = fig.legend(
        handles=handles,
        loc="lower center",
        bbox_to_anchor=(0.5, 0.075),
        ncol=len(handles),
        frameon=False,
        fontsize=9.5,
        handlelength=2.4,
        handletextpad=0.6,
        columnspacing=1.6,
    )
    for txt in legend.get_texts():
        txt.set_color(style.TEXT)


def _format_ratio(r: float) -> str:
    if not np.isfinite(r):
        return "n/a"
    if r < 0.95:
        return f"  {r:.2f}x faster"
    if r < 1.05:
        return "  baseline"
    if r < 10.0:
        return f"  {r:.1f}x slower"
    return f"  {r:.0f}x slower"


def _build_empty_figure(title: str, reason: str) -> Figure:
    fig = _new_svg_figure(figsize=(8.0, 4.0))
    ax = fig.subplots(1, 1)
    ax.set_title(f"{title}: no data", color=style.TEXT)
    ax.text(
        0.5,
        0.5,
        reason,
        fontsize=11,
        ha="center",
        va="center",
        transform=ax.transAxes,
        color=style.MUTED,
    )
    for spine in ax.spines.values():
        spine.set_visible(False)
    ax.set_xticks([])
    ax.set_yticks([])
    return fig


__all__ = [
    "WorkloadFigureInputs",
    "build_family_geomean_figure",
    "build_family_scatter_figure",
    "build_losses_figure",
    "build_overview_figure",
    "build_workload_figure",
]
