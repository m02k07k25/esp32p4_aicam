#!/usr/bin/env python3
from pathlib import Path
import shutil
import subprocess
import sys


HERE = Path(__file__).resolve().parent
SERVER = HERE.parent
ROOT = SERVER.parent.parent
BUILD = HERE / ".build"


def main() -> int:
    cc = shutil.which("gcc")
    if cc is None:
        print("gcc is required", file=sys.stderr)
        return 2
    BUILD.mkdir(exist_ok=True)
    output = BUILD / "reassembly_host_test.exe"
    command = [
        cc,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        f"-I{HERE / 'stubs'}",
        f"-I{SERVER / 'main'}",
        f"-I{ROOT / 'firmware' / 'components' / 'ble_mesh_image_protocol' / 'include'}",
        str(HERE / "reassembly_host_test.c"),
        str(SERVER / "main" / "image_reassembly.c"),
        "-o",
        str(output),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(output)], check=True)
    serial_output = BUILD / "serial_header_host_test.exe"
    serial_command = [
        cc,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        f"-I{HERE / 'stubs'}",
        f"-I{SERVER / 'main'}",
        str(HERE / "serial_header_host_test.c"),
        "-o",
        str(serial_output),
    ]
    subprocess.run(serial_command, check=True)
    subprocess.run([str(serial_output)], check=True)
    serial_time_output = BUILD / "serial_time_header_host_test.exe"
    serial_time_command = [
        cc,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        f"-I{HERE / 'stubs'}",
        f"-I{SERVER / 'main'}",
        str(HERE / "serial_time_header_host_test.c"),
        "-o",
        str(serial_time_output),
    ]
    subprocess.run(serial_time_command, check=True)
    subprocess.run([str(serial_time_output)], check=True)
    subprocess.run([sys.executable, str(HERE / "serial_protocol_test.py")],
                   check=True)
    subprocess.run([sys.executable, str(HERE / "static_gateway_checks.py")],
                   check=True)
    subprocess.run([sys.executable, str(HERE / "test_receive_images.py")],
                   check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
