"""Palette, tick lattice, and chrome constants for the plot_bench charts."""

import math
from collections.abc import Sequence

TEXT = "#111419"
MUTED = "#4E5866"
AXIS = "#2A313A"
GRID = "#E4E8ED"

BAND_WIN = "#117733"
BAND_LOSS = "#CC6677"

PALETTE: tuple[str, ...] = (
    "#0072B2",
    "#009E73",
    "#D55E00",
    "#CC79A7",
    "#56B4E9",
    "#E69F00",
    "#F0E442",
)

TIME_TICK_LATTICE_NS: tuple[float, ...] = (
    1.0,
    1e1,
    1e2,
    1e3,
    1e4,
    1e5,
    1e6,
    1e7,
    1e8,
    1e9,
    1e10,
    1e11,
)


def _half_decade_lattice(lattice: Sequence[float]) -> list[float]:
    """Interleave `2 x decade` and `5 x decade` between every pair of decades."""
    out: list[float] = []
    for v in lattice:
        out.extend([v, 2.0 * v, 5.0 * v])
    return sorted(set(out))


def pick_log_ticks(
    lattice: Sequence[float],
    lo: float,
    hi: float,
    *,
    target_max: int = 7,
) -> list[float]:
    """Pick major ticks from `lattice` inside `[lo, hi]`, thinned to `target_max`.

    Falls back to a half-decade lattice (`1, 2, 5, 10, 20, 50, ...`) when fewer
    than three decade ticks land in `[lo, hi]`; without the fallback narrow
    ranges like `[10us, 100us]` rendered with a single tick, leaving the axis
    near-blank.
    """
    in_range = [t for t in lattice if lo <= t <= hi]
    if len(in_range) < 3:
        dense = _half_decade_lattice(lattice)
        dense_in = [t for t in dense if lo <= t <= hi]
        if len(dense_in) >= len(in_range):
            in_range = dense_in
    if not in_range:
        below = [t for t in lattice if t < lo]
        above = [t for t in lattice if t > hi]
        bracket: list[float] = []
        if below:
            bracket.append(below[-1])
        if above:
            bracket.append(above[0])
        return bracket
    if len(in_range) <= target_max:
        return list(in_range)
    step = (len(in_range) - 1) / (target_max - 1)
    return [in_range[round(i * step)] for i in range(target_max)]


def fmt_time_ns(y: float, _pos: int = 0) -> str:
    """Format a nanosecond value with an SI unit for tick labels."""
    if y <= 0 or math.isnan(y):
        return ""
    if y < 1e3:
        return f"{y:g} ns"
    if y < 1e6:
        v = y / 1e3
        return f"{int(v)} us" if v == int(v) else f"{v:g} us"
    if y < 1e9:
        v = y / 1e6
        return f"{int(v)} ms" if v == int(v) else f"{v:g} ms"
    if y < 60.0 * 1e9:
        v = y / 1e9
        return f"{int(v)} s" if v == int(v) else f"{v:g} s"
    v = y / (60.0 * 1e9)
    return f"{int(v)} min" if v == int(v) else f"{v:g} min"


def pool_color(name: str, palette: Sequence[str] = PALETTE) -> str:
    """Pick a colour for a pool name. citor pools land on `BAND_WIN`; everything
    else maps to a stable slot in `palette` via FNV-1a so reruns produce the
    same competitor-to-colour assignment."""
    if name.startswith("citor::") or name == "citor":
        return BAND_WIN
    h = 2166136261
    for ch in name:
        h ^= ord(ch)
        h = (h * 16777619) & 0xFFFFFFFF
    return palette[h % len(palette)]


__all__ = [
    "AXIS",
    "BAND_LOSS",
    "BAND_WIN",
    "GRID",
    "MUTED",
    "PALETTE",
    "TEXT",
    "TIME_TICK_LATTICE_NS",
    "fmt_time_ns",
    "pick_log_ticks",
    "pool_color",
]
