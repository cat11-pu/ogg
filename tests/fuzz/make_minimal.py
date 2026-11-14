#!/usr/bin/env python3
"""Hand-built minimal counterexamples, one per malformed family.

Each file is the smallest byte sequence that triggers the named
diagnostic with an exact offset and reason.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ogg_model import crc32ogg  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "minimal")
os.makedirs(OUT, exist_ok=True)


def page(version=0, flags=0, granule=0, serial=0x1A2B3C4D, pageno=0,
         seg=(1,), body=b"A"):
    header = bytearray(27)
    header[0:4] = b"OggS"
    header[4] = version
    header[5] = flags
    struct.pack_into("<q", header, 6, granule)
    struct.pack_into("<I", header, 14, serial)
    struct.pack_into("<I", header, 18, pageno)
    header[26] = len(seg)
    raw = bytes(header) + bytes(seg) + bytes(body)
    crc = crc32ogg(raw)
    return raw[:22] + struct.pack("<I", crc) + raw[26:]


def write(name, blob):
    path = os.path.join(OUT, name)
    with open(path, "wb") as fh:
        fh.write(blob)
    print(f"{name}: {len(blob)} bytes")


def main():
    # 1. Truncation: complete capture + version byte, cut mid-header
    #    (smallest recognizable OggS fragment that still parses header
    #    fields is 27 bytes; use the first 10 for a minimal truncation).
    write("01_truncation.ogg", page()[:10])

    # 2. Page-length mismatch: header says one lacing byte of 200 but the
    #    buffer ends right after the segment table; declared body missing.
    p = page(seg=(200,), body=b"")
    write("02_page_length_mismatch.ogg", p)

    # 3. Bad checksum: structurally complete one-segment page with one
    #    CRC storage byte corrupted.
    p = bytearray(page(seg=(1,), body=b"A"))
    p[22] ^= 0xFF
    write("03_bad_checksum.ogg", bytes(p))

    # 4. Illegal version: valid page, version byte set to 1 (RFC only
    #    defines 0); checksum recomputed so version is the sole fault.
    p = bytearray(page(version=1, seg=(1,), body=b"A"))
    p[22:26] = b"\x00\x00\x00\x00"
    p[22:26] = struct.pack("<I", crc32ogg(bytes(p)))
    write("04_illegal_version.ogg", bytes(p))

    # 5. Cyclic page chain: two checksum-valid pages in one logical
    #    stream carrying the same page number (0, then 0 again).
    a = page(pageno=0, seg=(1,), body=b"A")
    b = page(pageno=0, seg=(1,), body=b"B")
    write("05_cyclic_page_chain.ogg", a + b)


if __name__ == "__main__":
    main()
