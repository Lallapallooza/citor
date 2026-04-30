"""Load `parallel_bench --export` JSON into a long-form DataFrame."""

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np
import pandas as pd

_SUPPORTED_SCHEMA = 1


@dataclass(frozen=True, slots=True)
class GateResult:
    name: str
    status: str
    detail: str


@dataclass(frozen=True, slots=True)
class Context:
    tool: str
    citor_version: str
    citor_commit: str
    citor_dirty: bool
    datetime_utc: str
    hostname: str
    kernel: str
    cpu_model: str
    cpu_logical: int
    compiler: str
    compiler_version: str
    build_type: str
    avx2: bool
    tsc_cycles_per_ns: float
    taskset_cpus: str
    checklist: tuple[GateResult, ...] = field(default_factory=tuple)


def load_export(path: Path) -> tuple[Context, pd.DataFrame]:
    """Parse a `parallel_bench --export` JSON document.

    Returns `(context, samples_df)` where `samples_df` is sorted by
    `(workload, pool, rep)`. Raises `ValueError` on schema mismatch or missing
    required fields.
    """
    raw = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError(f"{path}: top-level JSON value must be an object")
    schema_v = raw.get("schema_version")
    if schema_v != _SUPPORTED_SCHEMA:
        raise ValueError(
            f"{path}: unsupported schema_version {schema_v!r}; expected {_SUPPORTED_SCHEMA}"
        )
    ctx_raw = raw.get("context")
    if not isinstance(ctx_raw, dict):
        raise ValueError(f"{path}: missing or non-object 'context'")
    samples_raw = raw.get("samples")
    if not isinstance(samples_raw, list):
        raise ValueError(f"{path}: missing or non-array 'samples'")

    context = _context_from_dict(ctx_raw)
    df = _samples_from_list(samples_raw)
    return context, df


def _context_from_dict(d: dict[str, Any]) -> Context:
    checklist_raw = d.get("checklist", [])
    if not isinstance(checklist_raw, list):
        raise ValueError("context.checklist must be a list")
    gates = tuple(
        GateResult(
            name=str(g.get("name", "")),
            status=str(g.get("status", "UNKNOWN")),
            detail=str(g.get("detail", "")),
        )
        for g in checklist_raw
        if isinstance(g, dict)
    )
    return Context(
        tool=str(d.get("tool", "")),
        citor_version=str(d.get("citor_version", "")),
        citor_commit=str(d.get("citor_commit", "")),
        citor_dirty=bool(d.get("citor_dirty", False)),
        datetime_utc=str(d.get("datetime_utc", "")),
        hostname=str(d.get("hostname", "")),
        kernel=str(d.get("kernel", "")),
        cpu_model=str(d.get("cpu_model", "")),
        cpu_logical=int(d.get("cpu_logical", 0)),
        compiler=str(d.get("compiler", "")),
        compiler_version=str(d.get("compiler_version", "")),
        build_type=str(d.get("build_type", "")),
        avx2=bool(d.get("avx2", False)),
        tsc_cycles_per_ns=float(d.get("tsc_cycles_per_ns", 0.0)),
        taskset_cpus=str(d.get("taskset_cpus", "")),
        checklist=gates,
    )


def _samples_from_list(rows: list[Any]) -> pd.DataFrame:
    if not rows:
        return pd.DataFrame(
            {
                "workload": pd.Series(dtype="string"),
                "pool": pd.Series(dtype="string"),
                "rep": pd.Series(dtype="int64"),
                "cycles": pd.Series(dtype="uint64"),
                "ns": pd.Series(dtype="float64"),
            }
        )
    workloads: list[str] = []
    pools: list[str] = []
    reps: list[int] = []
    cycles: list[int] = []
    ns: list[float] = []
    for entry in rows:
        if not isinstance(entry, dict):
            raise ValueError(f"sample row must be an object: got {type(entry).__name__}")
        workloads.append(str(entry["workload"]))
        pools.append(str(entry["pool"]))
        reps.append(int(entry["rep"]))
        cycles.append(int(entry["cycles"]))
        ns.append(float(entry["ns"]))
    df = pd.DataFrame(
        {
            "workload": pd.Series(workloads, dtype="string"),
            "pool": pd.Series(pools, dtype="string"),
            "rep": pd.Series(reps, dtype="int64"),
            "cycles": pd.Series(cycles, dtype="uint64"),
            "ns": pd.Series(ns, dtype="float64"),
        }
    )
    df.sort_values(["workload", "pool", "rep"], kind="stable", inplace=True)
    df.reset_index(drop=True, inplace=True)
    return df


def bootstrap_median_ci(
    samples: np.ndarray, *, resamples: int = 1024, ci: float = 0.95, seed: int = 0xC1701
) -> tuple[float, float, float]:
    """Bootstrap a confidence interval for the median.

    Returns `(median, lo, hi)`. Returns `(median, nan, nan)` for `n < 12` since
    the percentile bootstrap is unreliable below that.
    """
    if samples.size == 0:
        return (float("nan"), float("nan"), float("nan"))
    median = float(np.median(samples))
    if samples.size < 12:
        return (median, float("nan"), float("nan"))
    rng = np.random.default_rng(seed)
    idx = rng.integers(0, samples.size, size=(resamples, samples.size))
    medians = np.median(samples[idx], axis=1)
    alpha = (1.0 - ci) / 2.0
    lo = float(np.quantile(medians, alpha))
    hi = float(np.quantile(medians, 1.0 - alpha))
    return (median, lo, hi)


def derive_family(workload: str) -> str:
    """The leading underscore-separated token of `workload`, lowercased.

    The bench names workloads `<family>_<shape>...` (`forkjoin_cilksort_j8`,
    `reduce_plus_int64_j16`), so the prefix is the family.
    """
    return workload.split("_", 1)[0].lower()


__all__ = [
    "Context",
    "GateResult",
    "bootstrap_median_ci",
    "derive_family",
    "load_export",
]
