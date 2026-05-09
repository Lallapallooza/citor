"""Read host invariants and emit a JSON disclosure block."""

import datetime
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path


def _read(path: str, default: str = "unknown") -> str:
    try:
        return Path(path).read_text(encoding="utf-8").strip()
    except OSError:
        return default


def _cpu_model() -> str:
    text = (
        Path("/proc/cpuinfo").read_text(encoding="utf-8") if Path("/proc/cpuinfo").is_file() else ""
    )
    for line in text.splitlines():
        if line.startswith("model name"):
            _, _, rest = line.partition(":")
            return rest.strip()
    return platform.processor() or "unknown"


def _distro() -> str:
    rel = Path("/etc/os-release")
    if not rel.is_file():
        return "unknown"
    for line in rel.read_text(encoding="utf-8").splitlines():
        if line.startswith("PRETTY_NAME="):
            return line.split("=", 1)[1].strip().strip('"')
    return "unknown"


def _git_sha(repo_root: Path) -> str:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repo_root, stderr=subprocess.DEVNULL
        )
        return out.decode().strip()
    except (FileNotFoundError, subprocess.CalledProcessError):
        return "unknown"


def disclosure(repo_root: Path, taskset_mask: str) -> dict[str, object]:
    """Build the host-disclosure dict written next to bench results."""
    return {
        "timestamp_iso8601": datetime.datetime.now(datetime.timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z"),
        "hostname": platform.node().split(".")[0],
        "kernel": platform.release(),
        "distro": _distro(),
        "cpu": _cpu_model(),
        "cpu_cores_logical": os.cpu_count() or 0,
        "governor": _read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
        "turbo_no_turbo": _read(
            "/sys/devices/system/cpu/intel_pstate/no_turbo",
            _read("/sys/devices/system/cpu/cpufreq/boost"),
        ),
        "smt_active": _read("/sys/devices/system/cpu/smt/active"),
        "aslr": _read("/proc/sys/kernel/randomize_va_space"),
        "taskset_mask": taskset_mask,
        "git_sha": _git_sha(repo_root),
    }


def write(path: Path, repo_root: Path, taskset_mask: str) -> None:
    path.write_text(json.dumps(disclosure(repo_root, taskset_mask), indent=2), encoding="utf-8")


def find_bench(default: str = "build/benchmark/parallel_bench") -> Path:
    """Resolve the parallel_bench binary. Honors `BENCH=` env var."""
    override = os.environ.get("BENCH")
    candidate = Path(override) if override else Path(default)
    if not candidate.is_file() or not os.access(candidate, os.X_OK):
        sys.stderr.write(
            f"error: {candidate} not found or not executable. Configure with "
            "-DCITOR_BUILD_BENCHMARK=ON and build the parallel_bench target.\n"
        )
        sys.exit(2)
    return candidate


def fail_if_running(name: str = "parallel_bench") -> None:
    """Error out when another bench process is alive on the host."""
    if (
        shutil.which("pgrep")
        and subprocess.call(["pgrep", "-x", name], stdout=subprocess.DEVNULL) == 0
    ):
        sys.stderr.write(f"error: another {name} is already running. Wait for it to finish.\n")
        sys.exit(2)
