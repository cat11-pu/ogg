#!/usr/bin/env python3
"""Generate a valid Ogg stream and malformed-page corpus samples.

The valid stream is built with the public encoder logic (manual framing with
correct CRC) so the parser output can be compared byte-for-byte before and
after the hardening change.  Every malformed sample is a single file that can
be fed straight into the fuzzing harness (fuzz_pagein.c).
"""

import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, "corpus")

CRC_POLY = 0x04C11DB7


def crc_table():
    table = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ CRC_POLY) & 0xFFFFFFFF if r & 0x80000000 \
                else (r << 1) & 0xFFFFFFFF
        table.append(r)
    return table


CRC = crc_table()


def ogg_crc(data):
    c = 0
    for b in data:
        c = ((c << 8) & 0xFFFFFFFF) ^ CRC[((c >> 24) & 0xFF) ^ b]
    return c


def lacing_for(sizes):
    """Segment table for a sequence of complete packet sizes."""
    segs = []
    for size in sizes:
        full, rem = divmod(size, 255)
        segs.extend([255] * full)
        segs.append(rem)
    return segs


def build_page(serial, pageno, segs, body, flags=0, granule=0,
               version=0, bad_crc=False, seg_count_override=None):
    nseg = seg_count_override if seg_count_override is not None else len(segs)
    header = bytearray(27)
    header[0:4] = b"OggS"
    header[4] = version
    header[5] = flags
    struct.pack_into("<q", header, 6, granule)
    struct.pack_into("<I", header, 14, serial & 0xFFFFFFFF)
    struct.pack_into("<I", header, 18, pageno & 0xFFFFFFFF)
    header[22:26] = b"\x00\x00\x00\x00"
    header[26] = nseg & 0xFF
    header.extend(s & 0xFF for s in segs)
    page = bytes(header) + bytes(body)
    crc = 0 if bad_crc else ogg_crc(page)
    out = bytearray(page)
    struct.pack_into("<I", out, 22, crc)
    return bytes(out)


def page_parts(pages):
    """Split a byte stream of pages into (header, body) pairs."""
    parts = []
    off = 0
    for raw in pages:
        nseg = raw[26]
        hlen = 27 + nseg
        blen = sum(raw[27 + i] for i in range(nseg))
        parts.append((bytearray(raw[:hlen]), bytearray(raw[hlen:hlen + blen])))
        off += hlen + blen
    return parts


def write(name, data):
    path = os.path.join(CORPUS, name)
    with open(path, "wb") as fh:
        fh.write(data)
    print("wrote %-28s %5d bytes" % (name, len(data)))


def main():
    os.makedirs(CORPUS, exist_ok=True)
    serial = 0x04030201

    # ---- valid reference stream: BOS packet, a 600-byte packet that spans
    # two pages, then a small EOS packet. -------------------------------
    packets = [bytes((j + 1) & 0xFF for j in range(n))
               for n in (17, 600, 10)]
    valid_pages = []
    pageno = 0

    # page 0: BOS, first packet (17 bytes) + start of the 600-byte packet
    segs0 = lacing_for([17]) + [255, 255]          # 510 bytes of packet 1
    body0 = packets[0] + packets[1][:510]
    valid_pages.append(build_page(
        serial, pageno, segs0, body0, flags=0x02, granule=0))
    pageno += 1

    # page 1: continuation of packet 1 (90 bytes) + full packet 2 (10)
    segs1 = [90] + lacing_for([10])
    body1 = packets[1][510:] + packets[2]
    valid_pages.append(build_page(
        serial, pageno, segs1, body1, flags=0x01, granule=627))
    pageno += 1
    write("valid_stream.ogg", b"".join(valid_pages))

    hdr, bdy = page_parts(valid_pages)

    # ---- class 1: corrupt pages ---------------------------------------
    # 1a: valid CRC but segment table promises more body bytes than present.
    segs = [255, 255, 10]                         # claims 520 body bytes
    corrupt_body = b"A" * 100                    # only 100 actually present
    write("corrupt_seg_overrun.bin",
          build_page(serial, 0, segs, corrupt_body, flags=0x02))

    # 1b: checksum mismatch (sync layer must skip, never crash)
    good = build_page(serial, 0, lacing_for([17]), packets[0], flags=0x02)
    bad = bytearray(good)
    bad[30] ^= 0xFF
    write("corrupt_bad_crc.bin", bytes(bad))

    # 1c: unsupported version field, valid CRC
    write("corrupt_bad_version.bin",
          build_page(serial, 0, lacing_for([17]), packets[0],
                     flags=0x02, version=1))

    # ---- class 2: cross-page continuation packets ---------------------
    # 2a: continuation flag with NO partial packet pending; segments claim a
    # body larger than what the page carries -> bodysize underflow path.
    segs = [255, 255, 200]
    body = b"B" * 300                             # table claims 710
    write("continue_orphan_overrun.bin",
          build_page(serial, 0, segs, body, flags=0x01))

    # 2b: continuation page whose leading segments alone exceed the body.
    segs = [255, 255, 255, 255, 1]
    body = b"C" * 512                             # table claims 1021
    write("continue_skip_underflow.bin",
          build_page(serial, 0, segs, body, flags=0x01))

    # 2c: valid continuation page for regression (matches valid page 1).
    write("continue_valid.bin",
          build_page(serial, 1, segs1, body1, flags=0x01, granule=627))

    # ---- class 3: wrong sequence numbers ------------------------------
    # 3a: duplicate page number (0 again after BOS already consumed 0).
    dup = bytearray(valid_pages[0])
    write("seq_duplicate.bin", bytes(dup))

    # 3b: backwards jump in page number, and the page itself is also
    # structurally inconsistent (lacing overrun) to prove the unroll loop
    # cannot drive body_fill negative.
    segs = [255, 255]
    body = b"D" * 40
    write("seq_backwards_overrun.bin",
          build_page(serial, 0, segs, body, flags=0x00))

    # 3c: hole in sequence (page 9 next, expected 1) but well-formed.
    write("seq_gap_valid.bin",
          build_page(serial, 9, lacing_for([10]), packets[2], granule=10))

    # ---- truncation / noise -------------------------------------------
    write("truncated_header.bin", b"OggS" + b"\x00" * 10)
    write("truncated_body.bin", good[:len(good) - 5])
    write("garbage_prefix.bin", b"XXXX garbage" + good)
    write("all_garbage.bin", b"\xff" * 64 + b"Ogg" + b"\x00" * 32)


if __name__ == "__main__":
    main()
