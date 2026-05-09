"""Smoke bench: a handful of representative cells."""

import argparse
import subprocess
import sys
import time

from tools.bench import host

CELLS = (
    "empty_fan_out_j16",
    "granularity_j16_body1us",
    "reduce_plus_int64_j16",
    "scan_inclusive_j16",
    "forkjoin_fib28_j16",
    "runplex_stencil_j16",
)


def add_parser(sub: argparse._SubParsersAction) -> None:
    p = sub.add_parser("quick", help="smoke bench (~90s)")
    p.add_argument("--taskset", default="0-15", help="taskset CPU mask")
    p.set_defaults(func=run)


def run(args: argparse.Namespace) -> int:
    bench = host.find_bench()
    host.fail_if_running()
    pattern = "|".join(CELLS)
    sys.stderr.write(f"quickbench filter: {pattern}\n")
    start = time.monotonic()
    rc = subprocess.call(["taskset", "-c", args.taskset, str(bench), "--filter", pattern])
    sys.stderr.write(f"elapsed: {int(time.monotonic() - start)}s\n")
    return rc
