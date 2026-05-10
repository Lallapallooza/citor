"""Per-workload isolated runner. Each cell runs in its own process so a
competitor's segfault does not kill the rest of the sweep."""

import argparse
import subprocess
import sys
from pathlib import Path

from tools.bench import host

REPO_ROOT = Path(__file__).resolve().parent.parent.parent


def add_parser(sub: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    p = sub.add_parser("isolated", help="run each workload in its own process")
    p.add_argument("--out", default="bench_out/per_workload", help="per-workload JSON output dir")
    p.add_argument("--taskset", default="0-15", help="taskset CPU mask")
    p.add_argument("--per-cell-timeout", type=int, default=1800, help="per-cell wall budget (s)")
    p.add_argument("--filter", default="", help="optional substring to scope the cell list")
    p.set_defaults(func=run)


def run(args: argparse.Namespace) -> int:
    bench = host.find_bench()
    host.fail_if_running()
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    listing = subprocess.check_output([str(bench), "--list"]).decode().splitlines()
    cells = [c.strip() for c in listing if c.strip() and (not args.filter or args.filter in c)]
    if not cells:
        sys.stderr.write("no workloads matched\n")
        return 1

    log = out_dir / "run.log"
    log.write_text("", encoding="utf-8")
    ok = bad = 0
    for cell in cells:
        json_path = out_dir / f"{cell}.json"
        txt_path = out_dir / f"{cell}.txt"
        json_path.unlink(missing_ok=True)
        cmd = [
            "taskset",
            "-c",
            args.taskset,
            "timeout",
            str(args.per_cell_timeout),
            str(bench),
            "--filter",
            cell,
            "--with-tail-percentiles",
            "--export",
            str(json_path),
        ]
        with txt_path.open("w", encoding="utf-8") as fp:
            rc = subprocess.call(cmd, stdout=fp, stderr=subprocess.STDOUT)
        size = json_path.stat().st_size if json_path.is_file() else 0
        verdict = "OK" if rc == 0 and size > 0 else "FAIL"
        line = f"[{verdict} rc={rc} {size}B] {cell}\n"
        with log.open("a", encoding="utf-8") as fp:
            fp.write(line)
        sys.stdout.write(line)
        if verdict == "OK":
            ok += 1
        else:
            bad += 1

    summary = f"===== done: {ok} ok, {bad} failed =====\n"
    with log.open("a", encoding="utf-8") as fp:
        fp.write(summary)
    sys.stdout.write(summary)
    return 0 if bad == 0 else 1
