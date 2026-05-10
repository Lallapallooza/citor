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
from matplotlib.colors import LinearSegmentedColormap, TwoSlopeNorm
from matplotlib.figure import Figure
from matplotlib.lines import Line2D
from matplotlib.patches import Rectangle
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

# Vendor prefixes used to canonicalise pool variant names to one row per
# competitor. A pool name `P` is canonicalised to the first prefix `K` such
# that `P == K`, `P.startswith(K + "[")`, `P.startswith(K + "::")`, or
# `P.startswith(K + " ")`. The list is intentionally one entry per vendor:
# `Leopard`, `Taskflow`, and `dispenso` cover their `::ThreadPool`,
# `::Subflow`, `::dispatch`, etc. sub-variants by prefix match. citor is
# special-cased into `citor::ThreadPool` (baseline) and `citor::PoolGroup`
# (excluded from peer comparisons via `_is_peer`).
_VENDOR_PREFIXES: tuple[str, ...] = (
    "citor::ThreadPool",
    "citor::PoolGroup",
    "BS::thread_pool",
    "Eigen::ThreadPool",
    "Leopard",
    "OpenMP",
    "Sequential",
    "Taskflow",
    "dispenso",
    "dp::thread_pool",
    "libfork",
    "oneTBB",
    "riften::Thiefpool",
    "task_thread_pool",
    "tmc::cpu_executor",
)

# Pool vendors that should not appear as peer competitors in geomean / scatter /
# losses charts. `Sequential` is a single-thread baseline that beats every
# parallel pool on tiny `forkjoin_fib28`-style cells; including it as a peer
# drags geomeans toward zero on small-recursion workloads. `citor::PoolGroup`
# canonicalises as its own vendor but it is still citor; excluding it keeps
# the losses chart from listing "citor::ThreadPool loses to citor::PoolGroup",
# and keeps the vs-competitors charts honest.
_EXCLUDED_PEER_VENDORS: frozenset[str] = frozenset({"Sequential", "citor::PoolGroup"})


def _is_peer(pool: str, citor_pool: str) -> bool:
    """Whether `pool` is a peer competitor for the citor baseline.

    Excludes the baseline itself, anything in `_EXCLUDED_PEER_VENDORS`, and
    any pool whose canonical name starts with `citor::` (catches future citor
    sub-pools the bench may add).
    """
    if pool == citor_pool:
        return False
    if pool in _EXCLUDED_PEER_VENDORS:
        return False
    return not pool.startswith("citor::")


def canonical_pool(name: str) -> str:
    """Map a pool variant name to its primary vendor prefix.

    Aggregates adapter / configuration variants:
      `BS::thread_pool::parallelFor`               -> `BS::thread_pool`
      `BS::thread_pool::submit_blocks x30`         -> `BS::thread_pool`
      `BS::thread_pool[chainAdapter]`              -> `BS::thread_pool`
      `Eigen::ThreadPool::Schedule x7`             -> `Eigen::ThreadPool`
      `OpenMP[citor::CancellationToken ...]`       -> `OpenMP`
      `citor::ThreadPool[Static]`                  -> `citor::ThreadPool`
      `citor::ThreadPool::parallelFor x7`          -> `citor::ThreadPool`

    Pool names that don't start with a known vendor prefix pass through
    unchanged so future pools surface visibly rather than silently grouping.
    """
    for prefix in _VENDOR_PREFIXES:
        if name == prefix:
            return prefix
        if (
            name.startswith(prefix + "[")
            or name.startswith(prefix + "::")
            or name.startswith(prefix + " ")
        ):
            return prefix
    return name


def _canonical_best_medians(by_pool: dict[str, float]) -> dict[str, float]:
    """Group medians by canonical vendor name; keep the fastest variant per group.

    Skips non-finite or non-positive medians.
    """
    out: dict[str, float] = {}
    for pool, ns in by_pool.items():
        if not (math.isfinite(ns) and ns > 0):
            continue
        canon = canonical_pool(pool)
        prev = out.get(canon)
        if prev is None or ns < prev:
            out[canon] = ns
    return out


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
            color = style.MUTED if 0.95 <= ratio < 1.05 else style.TEXT
            ax.annotate(
                label,
                xy=(1.005, y_idx),
                xycoords=("axes fraction", "data"),
                xytext=(4, 0),
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
    """Per-peer survival curves over (workload, peer) speedup ratios.

    For each canonical peer vendor, plot the empirical survival function of
    `peer_median / citor_median` across every workload where that peer
    competes. The Y value at X=k is the fraction of cells where citor is at
    least `k` times faster than that peer. The Y value at X=1.0 is the peer's
    win-rate (fraction of cells where citor wins).

    A flat-and-high curve means citor dominates the peer; a steep early drop
    means the peer is close to citor across the suite. The chart is one fixed
    size regardless of workload count -- it scales with peer count, not with
    cell count, which is why the previous one-row-per-workload overview was
    unreadable at 90+ workloads.
    """
    if samples.empty:
        return _build_empty_figure("citor benchmark overview", "no samples")

    medians_by_workload = _medians_by_workload(samples)
    by_peer: dict[str, list[float]] = collections.defaultdict(list)
    for by_pool in medians_by_workload.values():
        canonical = _canonical_best_medians(by_pool)
        citor_ns = canonical.get(baseline_pool_prefix)
        if citor_ns is None or citor_ns <= 0:
            continue
        for pool, ns in canonical.items():
            if not _is_peer(pool, baseline_pool_prefix):
                continue
            by_peer[pool].append(ns / citor_ns)

    if not by_peer:
        return _build_overview_empty()

    win_rate = {p: sum(1 for r in v if r > 1.0) / len(v) for p, v in by_peer.items()}
    peers = sorted(by_peer, key=lambda p: -win_rate[p])
    all_ratios = [r for v in by_peer.values() for r in v]
    lo = max(0.4, min(all_ratios) * 0.95)
    hi = min(150.0, max(all_ratios) * 1.10)

    fig = _new_svg_figure(figsize=(11.0, 5.8))
    gs = fig.add_gridspec(nrows=1, ncols=1, left=0.07, right=0.66, top=0.80, bottom=0.16)
    ax = fig.add_subplot(gs[0, 0])

    ax.set_xscale("log")
    ax.set_xlim(lo, hi)
    ax.axvspan(lo, 1.0, color=style.BAND_LOSS, alpha=0.07, zorder=0)
    ax.axvspan(1.0, hi, color=style.BAND_WIN, alpha=0.06, zorder=0)
    ax.axvline(1.0, color=_BASELINE_LINE_COLOR, linewidth=1.3, linestyle="--", alpha=0.6, zorder=1)

    for peer in peers:
        ratios = sorted(by_peer[peer], reverse=True)
        n = len(ratios)
        xs = [lo, *ratios, hi]
        ys = [1.0, *[i / n for i in range(n)], 0.0]
        ax.step(
            xs,
            ys,
            where="post",
            color=style.pool_color(peer),
            linewidth=1.6,
            alpha=0.9,
            zorder=3,
        )
        ax.plot(
            1.0,
            win_rate[peer],
            marker="o",
            markersize=5.5,
            markerfacecolor=style.pool_color(peer),
            markeredgecolor="white",
            markeredgewidth=0.9,
            zorder=4,
        )

    ax.set_ylim(-0.02, 1.02)
    ax.yaxis.set_major_locator(FixedLocator([0.0, 0.25, 0.5, 0.75, 1.0]))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{int(v * 100)}%"))
    ax.xaxis.set_minor_locator(NullLocator())
    ax.xaxis.set_major_locator(
        FixedLocator(style.pick_log_ticks([0.5, 0.7, 1.0, 2.0, 5.0, 10.0, 25.0, 100.0], lo, hi))
    )
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:g}x"))
    ax.set_xlabel(
        "speedup of citor over peer (median ratio)",
        fontsize=11.0,
        color=style.TEXT,
        labelpad=8,
    )
    ax.set_ylabel(
        "fraction of cells at this speedup or better",
        fontsize=11.0,
        color=style.TEXT,
        labelpad=8,
    )
    _style_minimal_axes(ax, draw_grid=True)

    handles = [
        Line2D(
            [0],
            [0],
            color=style.pool_color(p),
            linewidth=2.2,
            label=f"{p}    win {int(round(win_rate[p] * 100))}%   (n={len(by_peer[p])})",
        )
        for p in peers
    ]
    legend = fig.legend(
        handles=handles,
        loc="center left",
        bbox_to_anchor=(0.67, 0.50),
        frameon=False,
        fontsize=9.0,
        handlelength=2.2,
        labelspacing=0.45,
    )
    for txt in legend.get_texts():
        txt.set_color(style.TEXT)

    _draw_header(fig, "citor benchmark overview")
    _draw_subtitle(
        fig,
        context,
        suffix=f"{sum(len(v) for v in by_peer.values())} (workload, peer) cells",
    )
    _draw_header_rule(fig)
    _draw_caption(fig, context)

    return fig


def _build_overview_empty() -> Figure:
    return _build_empty_figure("citor benchmark overview", "no peer overlap")


def build_family_geomean_figure(
    samples: pd.DataFrame,
    context: Context,
    family: str,
    *,
    citor_pool: str = "citor::ThreadPool",
) -> Figure:
    """Geomean speedup of citor over each canonical competitor across `family`.

    Pool variants collapse to their vendor prefix: `BS::thread_pool::parallelFor`
    and `BS::thread_pool[chainAdapter]` aggregate as `BS::thread_pool`, with the
    fastest variant per workload representing the vendor. Citor variants
    (`citor::ThreadPool[Static]`, `citor::ThreadPool::runPlex`, ...) collapse to
    `citor::ThreadPool` so the baseline lookup succeeds across families that
    only ship tagged variants. `Sequential` is excluded as a peer.
    """
    medians_by_workload = _medians_by_workload(samples)
    speedups: dict[str, list[float]] = collections.defaultdict(list)
    for workload, by_pool in medians_by_workload.items():
        if derive_family(workload) != family:
            continue
        canonical = _canonical_best_medians(by_pool)
        citor_ns = canonical.get(citor_pool)
        if citor_ns is None:
            continue
        for pool, ns in canonical.items():
            if not _is_peer(pool, citor_pool):
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
    ax.barh(labels, speeds, color=colors)
    ax.axvline(1.0, color=_BASELINE_LINE_COLOR, linewidth=1.2, linestyle="--", alpha=0.6)
    ax.set_xscale("log")
    ax.xaxis.set_minor_locator(NullLocator())
    ax.xaxis.set_major_locator(
        FixedLocator([1.0, 1.5, 2.0, 3.0, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0])
    )
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:g}x"))
    ax.text(
        1.0,
        len(labels) - 0.4,
        "  parity (1x)",
        color=_BASELINE_LINE_COLOR,
        fontsize=8.5,
        fontstyle="italic",
        ha="left",
        va="bottom",
        zorder=5,
    )
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
    """Per-workload scatter for one primitive family. Each dot is a workload.

    Pool variants are canonicalised to their vendor prefix and `Sequential` is
    excluded so each peer vendor surfaces once per workload.
    """
    medians_by_workload = _medians_by_workload(samples)
    points: list[tuple[str, str, float, float]] = []
    for workload, by_pool in medians_by_workload.items():
        if derive_family(workload) != family:
            continue
        canonical = _canonical_best_medians(by_pool)
        citor_ns = canonical.get(citor_pool)
        if citor_ns is None:
            continue
        for pool, ns in canonical.items():
            if not _is_peer(pool, citor_pool):
                continue
            points.append((workload, pool, ns, citor_ns))

    if not points:
        return _build_empty_figure(f"{family}: per-workload scatter", "no overlapping workloads")

    fig = _new_svg_figure(figsize=(9.5, 6.5))
    gs = fig.add_gridspec(nrows=1, ncols=1, left=0.10, right=0.72, top=0.82, bottom=0.13)
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

    legend = fig.legend(
        loc="center left",
        bbox_to_anchor=(0.74, 0.50),
        fontsize=9.0,
        frameon=False,
        handlelength=1.6,
        handletextpad=0.5,
        labelspacing=0.45,
    )
    for txt in legend.get_texts():
        txt.set_color(style.TEXT)

    _draw_header(fig, f"{family}: per-workload scatter")
    _draw_subtitle(fig, context, suffix="below diagonal = citor faster")
    _draw_header_rule(fig)
    _draw_caption(fig, context)
    return fig


def build_family_heatmap_figure(  # noqa: PLR0912, PLR0915
    samples: pd.DataFrame,
    context: Context,
    *,
    citor_pool: str = "citor::ThreadPool",
) -> Figure:
    """Per-(family, peer) geomean speedup heatmap across the whole suite.

    Rows are primitive families (only those with at least one peer overlap),
    columns are canonical peer vendors. Cell colour is `log10(geomean
    speedup)` on a diverging palette centred at parity (1.0x); cell text is
    the speedup value. Empty cells (no overlap between this family and this
    peer) are hatched. Marginal strips along the top and right show the
    per-peer and per-family geomeans across the whole row / column.

    Complements the per-peer survival curves in `build_overview_figure`:
    survival curves answer "across the suite, how often / how much does citor
    win", and this chart answers "for this family, which peer is hardest".
    """
    medians_by_workload = _medians_by_workload(samples)
    cells: dict[tuple[str, str], list[float]] = collections.defaultdict(list)
    for workload, by_pool in medians_by_workload.items():
        canonical = _canonical_best_medians(by_pool)
        citor_ns = canonical.get(citor_pool)
        if citor_ns is None or citor_ns <= 0:
            continue
        family = derive_family(workload)
        for pool, ns in canonical.items():
            if not _is_peer(pool, citor_pool):
                continue
            cells[(family, pool)].append(ns / citor_ns)

    if not cells:
        return _build_empty_figure("citor benchmark family scorecard", "no peer overlap")

    families = sorted({fam for fam, _ in cells})
    peers = sorted({peer for _, peer in cells})

    geo: dict[tuple[str, str], float] = {}
    for key, ratios in cells.items():
        gm = _geomean(ratios)
        if math.isfinite(gm) and gm > 0:
            geo[key] = gm

    fam_geo: dict[str, float] = {
        fam: _geomean([geo[(fam, p)] for p in peers if (fam, p) in geo]) for fam in families
    }
    fam_geo = {fam: gm for fam, gm in fam_geo.items() if math.isfinite(gm) and gm > 0}
    peer_geo: dict[str, float] = {
        peer: _geomean([geo[(f, peer)] for f in families if (f, peer) in geo]) for peer in peers
    }
    peer_geo = {peer: gm for peer, gm in peer_geo.items() if math.isfinite(gm) and gm > 0}

    families = sorted(fam_geo, key=lambda f: -fam_geo[f])
    peers = sorted(peer_geo, key=lambda p: peer_geo[p])
    if not families or not peers:
        return _build_empty_figure("citor benchmark family scorecard", "no peer overlap")

    matrix = np.full((len(families), len(peers)), np.nan)
    for i, fam in enumerate(families):
        for j, peer in enumerate(peers):
            v = geo.get((fam, peer))
            if v is not None:
                matrix[i, j] = v

    fig_width = max(9.0, 0.8 * len(peers) + 4.0)
    fig_height = max(5.5, 0.42 * len(families) + 2.6)
    fig = _new_svg_figure(figsize=(fig_width, fig_height))
    gs = fig.add_gridspec(
        nrows=2,
        ncols=2,
        width_ratios=[len(peers), 1],
        height_ratios=[1, len(families)],
        left=0.18,
        right=0.91,
        top=0.83,
        bottom=0.16,
        wspace=0.04,
        hspace=0.04,
    )
    ax_top = fig.add_subplot(gs[0, 0])
    ax_main = fig.add_subplot(gs[1, 0], sharex=ax_top)
    ax_right = fig.add_subplot(gs[1, 1], sharey=ax_main)

    cmap = LinearSegmentedColormap.from_list(
        "citor_div",
        [style.BAND_LOSS, "#FFFFFF", style.BAND_WIN],
        N=256,
    )
    flat = [v for v in matrix.flatten() if math.isfinite(v) and v > 0]
    log_max = max(1.5, math.log10(max(flat))) if flat else 1.5
    log_min = min(-0.4, math.log10(min(flat))) if flat else -0.4
    norm = TwoSlopeNorm(vmin=log_min, vcenter=0.0, vmax=log_max)

    log_matrix = np.where(np.isfinite(matrix) & (matrix > 0), np.log10(matrix), np.nan)
    ax_main.imshow(log_matrix, cmap=cmap, norm=norm, aspect="auto", interpolation="nearest")

    for (i, j), cell in np.ndenumerate(matrix):
        cell_f = float(cell)
        if not math.isfinite(cell_f) or cell_f <= 0:
            ax_main.add_patch(
                Rectangle(
                    (j - 0.5, i - 0.5),
                    1,
                    1,
                    hatch="///",
                    facecolor=style.GRID,
                    edgecolor="white",
                    linewidth=0.4,
                    alpha=0.45,
                )
            )
            continue
        # White text only when the cell is dark enough for contrast; near
        # parity the cell is white-ish so use TEXT colour.
        light_cell = abs(math.log10(cell_f)) < 0.35
        ax_main.text(
            j,
            i,
            _fmt_speedup(cell_f),
            ha="center",
            va="center",
            fontsize=8.5,
            color=style.TEXT if light_cell else "#0A0E14",
            zorder=4,
        )

    ax_main.set_xticks(range(len(peers)))
    ax_main.set_xticklabels(peers, rotation=30, ha="right", fontsize=9.5, color=style.TEXT)
    ax_main.set_yticks(range(len(families)))
    ax_main.set_yticklabels(families, fontsize=10.0, color=style.TEXT)
    ax_main.tick_params(axis="both", colors=style.MUTED, length=0)
    for spine in ax_main.spines.values():
        spine.set_color(style.GRID)

    top_row = np.array([math.log10(peer_geo[p]) for p in peers]).reshape(1, -1)
    ax_top.imshow(top_row, cmap=cmap, norm=norm, aspect="auto", interpolation="nearest")
    for j, peer in enumerate(peers):
        ax_top.text(
            j,
            0,
            _fmt_speedup(peer_geo[peer]),
            ha="center",
            va="center",
            fontsize=8.0,
            color=style.TEXT,
            zorder=4,
        )
    ax_top.set_yticks([0])
    ax_top.set_yticklabels(
        [
            "all",
        ],
        fontsize=9.0,
        color=style.MUTED,
    )
    ax_top.tick_params(axis="x", labelbottom=False, length=0)
    ax_top.tick_params(axis="y", length=0)
    for spine in ax_top.spines.values():
        spine.set_color(style.GRID)

    right_col = np.array([math.log10(fam_geo[f]) for f in families]).reshape(-1, 1)
    ax_right.imshow(right_col, cmap=cmap, norm=norm, aspect="auto", interpolation="nearest")
    for i, fam in enumerate(families):
        ax_right.text(
            0,
            i,
            _fmt_speedup(fam_geo[fam]),
            ha="center",
            va="center",
            fontsize=8.5,
            color=style.TEXT,
            zorder=4,
        )
    ax_right.set_xticks([0])
    ax_right.set_xticklabels(["all"], fontsize=9.0, color=style.MUTED)
    ax_right.tick_params(axis="y", labelleft=False, length=0)
    ax_right.tick_params(axis="x", length=0)
    for spine in ax_right.spines.values():
        spine.set_color(style.GRID)

    _draw_header(fig, "citor family scorecard")
    _draw_subtitle(
        fig,
        context,
        suffix="geomean citor speedup vs canonical peer (log scale)",
    )
    _draw_header_rule(fig)
    _draw_caption(fig, context)
    return fig


def _fmt_speedup(v: float) -> str:
    if v >= 10.0:
        return f"{v:.0f}x"
    if v >= 2.0:
        return f"{v:.1f}x"
    return f"{v:.2f}x"


def build_losses_figure(
    samples: pd.DataFrame,
    context: Context,
    *,
    citor_pool: str = "citor::ThreadPool",
    top_n: int = 20,
) -> Figure:
    """Workloads where citor's median is worse than the best (canonical) competitor.

    `Sequential` is excluded so this surfaces real parallel-pool losses, not
    workloads where a single-thread baseline beats every parallel pool because
    the parallelism overhead exceeds the body cost.
    """
    medians_by_workload = _medians_by_workload(samples)
    losses: list[tuple[str, str, float]] = []
    for workload, by_pool in medians_by_workload.items():
        canonical = _canonical_best_medians(by_pool)
        citor_ns = canonical.get(citor_pool)
        if citor_ns is None:
            continue
        best_pool = None
        best_ns = float("inf")
        for pool, ns in canonical.items():
            if not _is_peer(pool, citor_pool):
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
    gs = fig.add_gridspec(nrows=1, ncols=1, left=0.46, right=0.97, top=0.82, bottom=0.16)
    ax = fig.add_subplot(gs[0, 0])
    ax.barh(labels, ratios, color=style.BAND_LOSS)
    ax.set_xscale("log")
    ax.set_xlim(left=1.0, right=max(ratios) * 1.05)
    ax.xaxis.set_minor_locator(NullLocator())
    ax.xaxis.set_major_locator(FixedLocator([1.0, 1.1, 1.25, 1.5, 2.0, 3.0, 5.0, 10.0, 25.0]))
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _p: f"{v:g}x"))
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
        out[str(key[0])][str(key[1])] = float(median)
    return out


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
        return f"{r:.2f}x faster"
    if r < 1.05:
        return "baseline"
    if r < 10.0:
        return f"{r:.1f}x slower"
    return f"{r:.0f}x slower"


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
    "build_family_heatmap_figure",
    "build_family_scatter_figure",
    "build_losses_figure",
    "build_overview_figure",
    "build_workload_figure",
]
