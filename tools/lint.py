#!/usr/bin/env python3
"""Formatting and static-analysis checks for DAWg's C++ code.

Usage:
    python tools/lint.py format          # check formatting (CI)
    python tools/lint.py format --fix    # reformat in place
    python tools/lint.py tidy <build>    # run clang-tidy using <build>/compile_commands.json

Tools come from tools/requirements.txt (pinned versions). They are looked up on PATH,
then in the .tools virtual environment created by:
    python -m venv .tools && .tools/Scripts/pip install -r tools/requirements.txt   (Windows)
    python -m venv .tools && .tools/bin/pip install -r tools/requirements.txt       (macOS / Linux)
"""

import argparse
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE_DIRS = ["engine", "app", "tests"]
HEADER_SUFFIXES = {".h", ".hpp"}
SOURCE_SUFFIXES = {".cpp", ".mm"}


def find_tool(name: str) -> str:
    found = shutil.which(name)
    if found:
        return found
    for candidate in (ROOT / ".tools" / "Scripts" / f"{name}.exe", ROOT / ".tools" / "bin" / name):
        if candidate.exists():
            return str(candidate)
    sys.exit(f"{name} not found. Install it with: pip install -r tools/requirements.txt")


def cpp_files(suffixes: set[str]) -> list[Path]:
    return sorted(p for d in SOURCE_DIRS for p in (ROOT / d).rglob("*") if p.suffix in suffixes)


def run_format(fix: bool) -> int:
    tool = find_tool("clang-format")
    files = [str(p) for p in cpp_files(HEADER_SUFFIXES | SOURCE_SUFFIXES)]
    args = [tool, "-i"] if fix else [tool, "--dry-run", "--Werror"]
    return subprocess.run(args + files).returncode


def run_tidy(build_dir: Path) -> int:
    tool = find_tool("clang-tidy")
    if not (build_dir / "compile_commands.json").exists():
        sys.exit(f"{build_dir}/compile_commands.json not found. Configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "
                 "and a Ninja or Makefile generator.")

    def check(path: Path) -> tuple[Path, int, str]:
        result = subprocess.run(
            [tool, "-p", str(build_dir), "--quiet", "--extra-arg=-Wno-unused-command-line-argument",
             "--extra-arg=-Wno-unknown-warning-option", str(path)],
            capture_output=True, text=True)
        return path, result.returncode, result.stdout + result.stderr

    failed = 0
    with ThreadPoolExecutor() as pool:
        for path, code, output in pool.map(check, cpp_files(SOURCE_SUFFIXES)):
            relevant = [line for line in output.splitlines()
                        if "warnings generated" not in line and not line.startswith("[")]
            if code != 0 or any("warning:" in line or "error:" in line for line in relevant):
                failed += 1
                print("\n".join(relevant))
            else:
                print(f"ok  {path.relative_to(ROOT)}")

    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    fmt = sub.add_parser("format", help="check or fix formatting")
    fmt.add_argument("--fix", action="store_true", help="reformat files in place")
    tidy = sub.add_parser("tidy", help="run clang-tidy")
    tidy.add_argument("build_dir", type=Path, help="build directory containing compile_commands.json")
    args = parser.parse_args()

    if args.command == "format":
        return run_format(args.fix)
    return run_tidy(args.build_dir.resolve())


if __name__ == "__main__":
    sys.exit(main())
