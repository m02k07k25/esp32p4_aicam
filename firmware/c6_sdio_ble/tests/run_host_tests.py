#!/usr/bin/env python3
"""Build and run the P4/C6 SDIO protocol host tests."""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
C6_DIR = TEST_DIR.parent
REPO_DIR = C6_DIR.parents[1]
BUILD_DIR = TEST_DIR / ".build"
C6_PROTOCOL_DIR = C6_DIR / "components" / "sdio_frame_protocol" / "include"
BLE_PROTOCOL_DIR = REPO_DIR / "firmware" / "components" / "ble_mesh_image_protocol" / "include"
P4_PROTOCOL_DIR = (
    REPO_DIR
    / "firmware"
    / "p4_inference"
    / "components"
    / "sdio_frame_protocol"
    / "include"
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command: list[str]) -> None:
    print("+", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, check=True, cwd=TEST_DIR)


def main() -> int:
    gcc = shutil.which("gcc")
    if gcc is None:
        print("error: gcc is required for the host tests", file=sys.stderr)
        return 2

    c6_header = C6_PROTOCOL_DIR / "sdio_frame_protocol.h"
    p4_header = P4_PROTOCOL_DIR / "sdio_frame_protocol.h"
    if not c6_header.is_file() or not p4_header.is_file():
        print("error: a production protocol header is missing", file=sys.stderr)
        return 2

    c6_hash = digest(c6_header)
    p4_hash = digest(p4_header)
    if c6_hash != p4_hash:
        print("error: P4 and C6 protocol headers differ", file=sys.stderr)
        print(f"  P4: {p4_hash}", file=sys.stderr)
        print(f"  C6: {c6_hash}", file=sys.stderr)
        return 1
    print(f"protocol headers identical: sha256={c6_hash}")

    BUILD_DIR.mkdir(exist_ok=True)
    compile_flags = [
        gcc,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-O0",
        "-g",
    ]

    for target, include_dir in (
        ("layout_p4", P4_PROTOCOL_DIR),
        ("layout_c6", C6_PROTOCOL_DIR),
    ):
        executable = BUILD_DIR / f"{target}.exe"
        run(
            compile_flags
            + [
                f"-I{include_dir}",
                str(TEST_DIR / "protocol_layout_test.c"),
                "-o",
                str(executable),
            ]
        )
        run([str(executable)])

    receiver_executable = BUILD_DIR / "receiver_host_test.exe"
    run(
        compile_flags
        + [
            f"-I{TEST_DIR / 'stubs'}",
            f"-I{C6_DIR / 'main'}",
            f"-I{C6_PROTOCOL_DIR}",
            str(TEST_DIR / "receiver_host_test.c"),
            "-o",
            str(receiver_executable),
        ]
    )
    run([str(receiver_executable)])

    mesh_source_executable = BUILD_DIR / "mesh_source_host_test.exe"
    run(
        compile_flags
        + [
            f"-I{TEST_DIR / 'stubs'}",
            f"-I{C6_DIR / 'main'}",
            f"-I{BLE_PROTOCOL_DIR}",
            str(TEST_DIR / "mesh_source_host_test.c"),
            "-o",
            str(mesh_source_executable),
        ]
    )
    run([str(mesh_source_executable)])
    print("all host protocol tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
