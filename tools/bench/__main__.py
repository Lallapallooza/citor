"""`python -m tools.bench` entry point."""

import argparse
import sys

from tools.bench import diff, isolated, packaging, quick, run


def main() -> int:
    parser = argparse.ArgumentParser(prog="python -m tools.bench")
    sub = parser.add_subparsers(dest="cmd", required=True)
    for module in (run, quick, isolated, diff, packaging):
        module.add_parser(sub)
    args = parser.parse_args()
    return int(args.func(args))


if __name__ == "__main__":
    sys.exit(main())
