#!/usr/bin/env python3
import binascii
import struct


FORMAT = "<8sHHHBBQIIII"
MAGIC = b"BMJPEG01"
TIME_FORMAT = "<8sHHQII"
TIME_MAGIC = b"BMTIME01"


def main() -> None:
    assert struct.calcsize(FORMAT) == 40
    first_36 = struct.pack(
        "<8sHHHBBQIII",
        MAGIC,
        1,
        40,
        0x1234,
        0,
        0,
        1_725_000_000_123,
        30_720,
        0x89ABCDEF,
        7,
    )
    assert len(first_36) == 36
    header_crc = binascii.crc32(first_36) & 0xFFFFFFFF
    wire = first_36 + struct.pack("<I", header_crc)
    values = struct.unpack(FORMAT, wire)
    assert values[0] == MAGIC
    assert values[1:3] == (1, 40)
    assert (binascii.crc32(wire[:36]) & 0xFFFFFFFF) == values[-1]

    time_prefix = struct.pack(
        "<8sHHQI", TIME_MAGIC, 1, 28, 1_725_000_000_123, 9
    )
    assert len(time_prefix) == 24
    time_wire = time_prefix + struct.pack(
        "<I", binascii.crc32(time_prefix) & 0xFFFFFFFF
    )
    assert struct.calcsize(TIME_FORMAT) == 28
    time_values = struct.unpack(TIME_FORMAT, time_wire)
    assert time_values[:3] == (TIME_MAGIC, 1, 28)
    assert time_values[3:5] == (1_725_000_000_123, 9)
    assert (binascii.crc32(time_wire[:24]) & 0xFFFFFFFF) == time_values[-1]
    print("PASS: serial image/time LE formats and CRC coverage")


if __name__ == "__main__":
    main()
