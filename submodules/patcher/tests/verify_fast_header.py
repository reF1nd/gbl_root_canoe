#!/usr/bin/env python3

import struct
import sys
import zlib


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <fast_header> <patched_efi>")
        return 2

    with open(sys.argv[1], "rb") as source:
        header = source.read()
    with open(sys.argv[2], "rb") as source:
        image = source.read()

    if len(header) != 4096:
        raise SystemExit("fast header must be 4096 bytes")

    magic, version, header_bytes, image_offset, image_bytes, image_crc, header_crc, reserved = struct.unpack_from(
        "<8sIIQIIII", header
    )
    checked_header = bytearray(header[:40])
    checked_header[32:36] = b"\0" * 4

    assert magic == b"SFBFAST1"
    assert version == 1
    assert header_bytes == len(header)
    assert image_offset == 0x00081000
    assert image_bytes == len(image)
    assert image_crc == zlib.crc32(image)
    assert header_crc == zlib.crc32(checked_header)
    assert reserved == 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
