#!/usr/bin/env python3
import binascii
import struct


FORMAT = "<8sHHHBBQIIII"
MAGIC = b"BMJPEG01"


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
    print("PASS: serial LE format and first-36-byte header CRC")


if __name__ == "__main__":
    main()
