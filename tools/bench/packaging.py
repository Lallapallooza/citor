"""Run the packaging consume tests against the local repo."""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent


def add_parser(sub: "argparse._SubParsersAction[argparse.ArgumentParser]") -> None:
    p = sub.add_parser("packaging", help="run every packaging consume test")
    p.set_defaults(func=run)


def _cmake_consume(label: str, source_dir: Path, work: Path, extra: list[str]) -> None:
    print(f"== {label} ==")
    build_dir = work / label
    log = work / f"{label}.log"
    cmd_cfg = [
        "cmake",
        "-S",
        str(source_dir),
        "-B",
        str(build_dir),
        "-G",
        "Ninja",
        f"-DCITOR_LOCAL_SOURCE={REPO_ROOT}",
        *extra,
    ]
    with log.open("w") as fp:
        subprocess.check_call(cmd_cfg, stdout=fp, stderr=subprocess.STDOUT)
        subprocess.check_call(
            ["cmake", "--build", str(build_dir), "-j"], stdout=fp, stderr=subprocess.STDOUT
        )
    subprocess.check_call([str(build_dir / label)])
    print(f"PASS: {label}")


def run(args: argparse.Namespace) -> int:
    pkg_root = REPO_ROOT / "tests" / "packaging"
    work = Path(tempfile.mkdtemp(prefix="citor-packaging."))
    try:
        subprocess.check_call([sys.executable, str(REPO_ROOT / "tools" / "amalgamate.py")])

        cxx = os.environ.get("CXX", "c++")
        single_header_bin = work / "single_header_consume"
        print("== single-header ==")
        subprocess.check_call(
            [
                cxx,
                "-std=c++20",
                "-O2",
                "-pthread",
                "-I",
                str(REPO_ROOT / "single_include"),
                str(pkg_root / "single_header_consume" / "main.cpp"),
                "-o",
                str(single_header_bin),
            ]
        )
        subprocess.check_call([str(single_header_bin)])
        print("PASS: single-header")

        _cmake_consume("fetchcontent_consume", pkg_root / "fetchcontent_consume", work, [])
        _cmake_consume("cpm_consume", pkg_root / "cpm_consume", work, [])
        _cmake_consume("add_subdirectory_consume", pkg_root / "add_subdirectory_consume", work, [])

        print("== find_package ==")
        install_build = work / "install-build"
        install_prefix = work / "install-prefix"
        cfg = [
            "cmake",
            "-S",
            str(REPO_ROOT),
            "-B",
            str(install_build),
            "-G",
            "Ninja",
            "-DCITOR_BUILD_TESTS=OFF",
            "-DCITOR_BUILD_BENCHMARK=OFF",
            f"-DCMAKE_INSTALL_PREFIX={install_prefix}",
        ]
        with (work / "install.log").open("w") as fp:
            subprocess.check_call(cfg, stdout=fp, stderr=subprocess.STDOUT)
            subprocess.check_call(
                ["cmake", "--build", str(install_build), "-j"], stdout=fp, stderr=subprocess.STDOUT
            )
            subprocess.check_call(
                ["cmake", "--install", str(install_build)], stdout=fp, stderr=subprocess.STDOUT
            )
        _cmake_consume(
            "find_package_consume",
            pkg_root / "find_package_consume",
            work,
            [f"-DCMAKE_PREFIX_PATH={install_prefix}"],
        )

        if shutil.which("conan"):
            print("== conan ==")
            with (work / "conan.log").open("w") as fp:
                subprocess.call(
                    ["conan", "profile", "detect", "--force"], stdout=fp, stderr=subprocess.STDOUT
                )
                subprocess.check_call(
                    ["conan", "create", str(REPO_ROOT / "packaging" / "conan"), "--build=missing"],
                    stdout=fp,
                    stderr=subprocess.STDOUT,
                )
            print("PASS: conan")
        else:
            print("SKIP: conan (binary not on PATH)")

        # vcpkg port pins SHA512 against a tagged release tarball; runs in CI only.
        print("SKIP: vcpkg (CI-only against a tagged release)")

        print()
        print("all packaging consume tests passed")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)
