"""Full bench sweep. Wraps `parallel_bench` with host disclosure + taskset."""

import argparse
import subprocess
import sys
from pathlib import Path

from tools.bench import host

REPO_ROOT = Path(__file__).resolve().parent.parent.parent


def add_parser(sub: argparse._SubParsersAction[argparse.ArgumentParser]) -> None:
    p = sub.add_parser("run", help="full bench sweep")
    p.add_argument("--filter", default="", help="workload-name substring filter")
    p.add_argument("--out", default="", help="output dir (default: bench_out/<host>/<sha>)")
    p.add_argument("--taskset", default="0-15", help="taskset CPU mask (default: 0-15)")
    p.add_argument(
        "extra", nargs=argparse.REMAINDER, help="trailing args forwarded to parallel_bench"
    )
    p.set_defaults(func=run)


def run(args: argparse.Namespace) -> int:
    bench = host.find_bench()
    host.fail_if_running()

    if args.out:
        out_dir = Path(args.out)
    else:
        info = host.disclosure(REPO_ROOT, args.taskset)
        sha = str(info["git_sha"])[:12]
        out_dir = REPO_ROOT / "bench_out" / str(info["hostname"]) / sha
    out_dir.mkdir(parents=True, exist_ok=True)
    host.write(out_dir / "host.json", REPO_ROOT, args.taskset)

    cmd: list[str] = ["taskset", "-c", args.taskset, str(bench)]
    if args.filter:
        cmd += ["--filter", args.filter]
    cmd += ["--export", str(out_dir / "results.json")]
    extra = [a for a in (args.extra or []) if a != "--"]
    cmd += extra

    sys.stderr.write(f"running: {' '.join(cmd)}\nout:     {out_dir}\n")
    return subprocess.call(cmd)
