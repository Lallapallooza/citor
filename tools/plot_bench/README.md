# plot_bench

Render SVG charts from `parallel_bench --export` JSON.

## Usage

```bash
taskset -c 0-15 ./build/benchmark/parallel_bench --export bench.json
uv run python -m tools.plot_bench --input bench.json --output charts/
```

The dependencies are matplotlib, numpy, and pandas. They live under the
`dev` group in `pyproject.toml`; `uv sync --group dev` pulls them in.

## What gets written

Per invocation, into `--output`:

| File | Contents |
|---|---|
| `<workload>.svg` | Per-pool strip chart for one workload: faded raw dots, bootstrap CI bar, median diamond. Log time axis. |
| `_overview.svg` | One row per workload, pools as dots laid out by their median ratio against the citor baseline. |
| `_family_<family>_geomean.svg` | Geomean speedup of citor over each competitor across all workloads in one primitive family. |
| `_family_<family>_scatter.svg` | Scatter for one family: citor on Y, competitor on X, log-log, identity diagonal. |
| `_losses.svg` | Top 20 workloads where citor's median is worse than the best competitor, sorted descending. |

Pass `--no-per-workload`, `--no-overview`, `--no-family`, `--no-losses` to
suppress any of the categories.

The "family" of a workload is the leading underscore-separated token in its
name (`forkjoin_cilksort_j8` is family `forkjoin`).

## Flags

| Flag | Default | Effect |
|---|---|---|
| `--input PATH` | required | JSON file from `parallel_bench --export`. |
| `--output DIR` | required | Output directory. Created if missing. |
| `--baseline PREFIX` | `citor::ThreadPool` | Pool-name prefix used as the baseline. |
| `--filter SUBSTR` | (off) | Only render workloads matching SUBSTR. Repeatable; OR-ed. |
| `--no-per-workload` | off | Skip per-workload strip charts. |
| `--no-overview` | off | Skip the cross-workload overview. |
| `--no-family` | off | Skip family geomean and scatter charts. |
| `--no-losses` | off | Skip the losses summary. |

## Notes

- 95% confidence intervals on the per-workload chart are percentile bootstrap
  with 1024 resamples. For `n < 12` the CI is omitted (the percentile
  bootstrap is unreliable below that).
- Two invocations on the same JSON produce byte-identical SVGs: deterministic
  RNG seeds, fixed font sizes, no `pyplot` (matplotlib OO API only).
- citor pools render in forest green; competitors get an FNV-1a hash slot
  from a fixed palette so reruns assign the same competitor to the same
  colour.
