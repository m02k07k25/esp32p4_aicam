#!/usr/bin/env python3
"""Build and run P4 server-clock mapping tests with the host C compiler."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
P4_DIR = TEST_DIR.parent
BUILD_DIR = TEST_DIR / ".build"


def main() -> int:
    gcc = shutil.which("gcc")
    if gcc is None:
        print("error: gcc is required for the host tests", file=sys.stderr)
        return 2

    BUILD_DIR.mkdir(exist_ok=True)
    executable = BUILD_DIR / "sdio_time_clock_host_test.exe"
    command = [
        gcc,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-O0",
        "-g",
        f"-I{P4_DIR / 'main'}",
        f"-I{P4_DIR / 'components' / 'sdio_frame_protocol' / 'include'}",
        str(P4_DIR / "main" / "sdio_time_clock.c"),
        str(TEST_DIR / "sdio_time_clock_host_test.c"),
        "-o",
        str(executable),
    ]
    print("+", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, check=True, cwd=TEST_DIR)
    subprocess.run([str(executable)], check=True, cwd=TEST_DIR)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
