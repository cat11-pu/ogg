#!/usr/bin/env python3
"""Generate 200 valid and 2000 malformed Ogg bitstreams."""

import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ogg_model import (  # noqa: E402
    PageBuilder, BOS, EOS, CONTINUED, crc32ogg, serial_u32,
)

HERE = os.path.dirname(os.path.abspath(__file__))
VALID_DIR = os.path.join(HERE, "cases", "valid")
BAD_DIR = os.path.join(HERE, "cases", "malformed")


def build_valid_stream(rng, serial, npackets):
    """One logical bitstream: BOS packet, middle packets, EOS packet."""
    builder = PageBuilder(serial)
    packets = []
    for i in range(npackets):
        size_pool = [0, 1, 10, 254, 255, 256, 1000, 65025, 65026, 130051]
        size = rng.choice(size_pool)
        payload = bytes(rng.getrandbits(8) for _ in range(size))
        flags = 0
        if i == 0:
            flags |= BOS
        if i == npackets - 1:
            flags |= EOS
        granule = (i + 1) * rng.randint(1, 1000)
        packets.append((payload, flags, granule))
    # Vary page fill: sometimes force one packet per page, sometimes pack.
    max_seg = rng.choice([1, 2, 10, 255])
    return builder.pack(packets, max_segments=max_seg)


def gen_valid(idx, rng):
    nstreams = rng.choice([1, 1, 1, 2, 3])
    per_stream = rng.randint(1, 12)
    streams = []
    base_serial = rng.randint(0, 0xFFFFFFFF)
    for s in range(nstreams):
        pages = build_valid_stream(rng, serial_u32(base_serial + s),
                                   per_stream)
        streams.append(pages)
    # Interleave pages round-robin so the driver must demux serials.
    out = bytearray()
    i = 0
    while any(i < len(p) for p in streams):
        for pages in streams:
            if i < len(pages):
                out.extend(pages[i])
        i += 1
    # Occasional leading/trailing junk that a real decoder must skip.
    if rng.random() < 0.3:
        out = bytearray(rng.randrange(256) for _ in range(rng.randint(1, 8))) + out
    if rng.random() < 0.2:
        out.extend(rng.randrange(256) for _ in range(rng.randint(1, 8)))
    return bytes(out)


def one_base_page(rng, serial=0x1A2B3C4D, pageno=0, nseg=None,
                  payload_pattern=0xAB, flags=0, granule=0):
    """Build a single well-formed page with controllable shape."""
    if nseg is None:
        nseg = rng.randint(1, 8)
    seg = [rng.randint(0, 255) for _ in range(nseg)]
    body = bytes([payload_pattern]) * sum(seg)
    header = bytearray(27)
    header[0:4] = b"OggS"
    header[4] = 0
    header[5] = flags
    struct.pack_into("<q", header, 6, granule)
    struct.pack_into("<I", header, 14, serial)
    struct.pack_into("<I", header, 18, pageno)
    header[26] = nseg
    page = bytes(header) + bytes(seg) + body
    crc = crc32ogg(page)
    return page[:22] + struct.pack("<I", crc) + page[26:]


def multi_page_chain(rng, npages, serial=0x1A2B3C4D):
    pages = []
    for i in range(npages):
        flags = BOS if i == 0 else (EOS if i == npages - 1 else 0)
        pages.append(one_base_page(rng, serial=serial, pageno=i,
                                   flags=flags))
    return b"".join(pages)


# ---------- malformed families ----------

def gen_truncation(rng):
    page = one_base_page(rng)
    cut_choices = [
        1, 3, 4, 26,                       # inside/capture/header
        len(page) - 1,                     # one byte short
        len(page) - rng.randint(1, 27),    # header/seg-table tail
        rng.randint(27, max(28, len(page))),
    ]
    cut = rng.choice(cut_choices)
    cut = max(1, min(cut, len(page)))
    blob = page[:cut]
    if rng.random() < 0.4:  # prefix a valid page so resync is exercised
        blob = one_base_page(rng, serial=rng.randint(0, 0xFFFFFFFF)) + blob
    return blob, "truncation"


def gen_page_length(rng):
    """Declared page length disagrees with the buffered bytes.

    Two sub-classes, each individually checksum-valid up to the point
    where length metadata lies, so the failure is a length desync rather
    than a CRC rejection:
      * inflate a lacing value / segment count, then re-checksum; the
        declared body now runs past the end of the buffer;
      * truncate a fully valid page at a byte inside its declared body.
    """
    page = bytearray(one_base_page(rng))
    nseg = page[26]
    mode = rng.choice(["inflate_seg", "nseg_overflow", "truncate_body",
                       "truncate_body", "inflate_seg"])
    if mode in ("inflate_seg", "nseg_overflow"):
        if mode == "inflate_seg":
            idx = rng.randrange(nseg)
            old = page[27 + idx]
            page[27 + idx] = min(255, old + rng.randint(1, 255 - old)
                                  if old < 255 else 255)
            if page[27 + idx] == old:
                page[27 + idx] = 255
        else:
            page[26] = rng.choice([nseg + 1, 100, 200, 255])
        # Re-checksum the mutated page itself; its declared total length
        # now exceeds the bytes that actually follow in the buffer.
        page[22:26] = b"\x00\x00\x00\x00"
        page[22:26] = struct.pack("<I", crc32ogg(bytes(page)))
        blob = bytes(page)
    else:
        # Keep a fully valid page but cut it within the declared body, so
        # header+segtable are intact and checksum cannot even be evaluated
        # against a complete page.
        header_len = 27 + nseg
        body_len = len(page) - header_len
        keep = header_len + rng.randint(0, max(0, body_len - 1))
        blob = bytes(page[:keep])
    if rng.random() < 0.3:
        blob = one_base_page(rng, serial=rng.randint(0, 0xFFFFFFFF)) + blob
    return blob, "page_length_mismatch"


def gen_bad_checksum(rng):
    page = bytearray(one_base_page(rng))
    mode = rng.choice(["flip_byte", "wrong_crc_field", "body_corrupt",
                       "header_corrupt"])
    if mode == "flip_byte":
        # Keep the 4-byte capture pattern and the segment count intact
        # so the page is still recognizable; only payload/header/CRC
        # bytes get corrupted.
        idx = rng.randrange(4, len(page))
        if idx == 26:
            idx = 27
        if idx in (22, 23, 24, 25):
            page[idx] ^= 0xFF
        else:
            page[idx] ^= 1 << rng.randrange(8)
    elif mode == "wrong_crc_field":
        page[22 + rng.randrange(4)] = rng.randrange(256)
    elif mode == "body_corrupt":
        page[rng.randrange(27, len(page))] ^= 0x5A
    else:
        page[rng.randrange(6, 22)] ^= 0x3C
    # Unconditionally force an invalid checksum on the mutated page.
    # The CRC is defined over the declared page (header through the
    # lacing-declared body); recompute on exactly that span and store
    # the bitwise complement so verification always fails.
    nseg = page[26]
    declared = 27 + nseg + sum(page[27:27 + nseg])
    page[22:26] = b"\x00\x00\x00\x00"
    span = bytes(page[:declared])
    bad = (~crc32ogg(span)) & 0xFFFFFFFF
    page[22:26] = struct.pack("<I", bad)
    # Prefix a fully valid page (after the bad CRC is fixed in place) so
    # resync behavior is exercised without disturbing this page's CRC.
    prefix = b""
    if rng.random() < 0.3:
        prefix = one_base_page(rng, serial=rng.randint(0, 0xFFFFFFFF))
    blob = prefix + bytes(page)
    return blob, "bad_checksum"


def crc_of(blob):
    zeroed = blob[:22] + b"\x00\x00\x00\x00" + blob[26:]
    return crc32ogg(zeroed)


def gen_illegal_version(rng):
    npages = rng.randint(1, 3)
    pages = [bytearray(one_base_page(rng, pageno=i)) for i in range(npages)]
    target = rng.randrange(npages)
    pages[target][4] = rng.choice([1, 2, 0x7F, 0xFF])
    # Re-checksum: structural page is fine, only version violates RFC.
    pages[target][22:26] = b"\x00\x00\x00\x00"
    crc = crc32ogg(bytes(pages[target]))
    pages[target][22:26] = struct.pack("<I", crc)
    return b"".join(bytes(p) for p in pages), "illegal_version"


def gen_cyclic_chain(rng):
    """Reuse page numbers / serials so the chain never terminates."""
    mode = rng.choice(["repeated_pageno", "wrapping_pageno",
                       "backward_jump", "serial_recycle", "self_loop"])
    serial = rng.randint(1, 0xFFFFFFFE)
    if mode == "repeated_pageno":
        pages = [bytearray(one_base_page(rng, serial=serial, pageno=5))
                 for _ in range(rng.randint(2, 8))]
    elif mode == "wrapping_pageno":
        # Legal u32 wrap (FE -> FF -> 0 -> 1), then an illegal repeat
        # back onto page 0 so the chain actually cycles.
        seq = [0xFFFFFFFE, 0xFFFFFFFF, 0, 1, 0, 1, 0]
        pages = [bytearray(one_base_page(rng, serial=serial, pageno=p))
                 for p in seq[:rng.randint(5, 7)]]
    elif mode == "backward_jump":
        seq = [10, 11, 12, 3, 4, 12, 0]
        cut = rng.randint(4, 7)  # always includes the jump back to 3
        pages = [bytearray(one_base_page(rng, serial=serial, pageno=p))
                 for p in seq[:cut]]
    elif mode == "serial_recycle":
        # Same serial reused for a "fresh" BOS mid-stream.
        pages = [
            bytearray(one_base_page(rng, serial=serial, pageno=0, flags=BOS)),
            bytearray(one_base_page(rng, serial=serial, pageno=1)),
            bytearray(one_base_page(rng, serial=serial, pageno=0, flags=BOS)),
            bytearray(one_base_page(rng, serial=serial, pageno=1, flags=EOS)),
        ]
    else:
        page = bytearray(one_base_page(rng, serial=serial, pageno=7,
                                       flags=EOS | CONTINUED))
        pages = [page] * rng.randint(2, 5)
        pages = [bytearray(p) for p in pages]
    for p in pages:  # keep each page individually checksum-valid
        p[22:26] = b"\x00\x00\x00\x00"
        p[22:26] = struct.pack("<I", crc32ogg(bytes(p)))
    return b"".join(bytes(p) for p in pages), "cyclic_page_chain"


MALFORMED = {
    "truncation": gen_truncation,
    "page_length_mismatch": gen_page_length,
    "bad_checksum": gen_bad_checksum,
    "illegal_version": gen_illegal_version,
    "cyclic_page_chain": gen_cyclic_chain,
}


def main():
    rng = random.Random(0x0663602)
    for d in (VALID_DIR, BAD_DIR):
        os.makedirs(d, exist_ok=True)
    manifest = []

    for i in range(200):
        blob = gen_valid(i, rng)
        name = f"valid_{i:04d}.ogg"
        with open(os.path.join(VALID_DIR, name), "wb") as fh:
            fh.write(blob)
        manifest.append((name, "valid", len(blob)))

    per_family = 400
    for family, fn in MALFORMED.items():
        for i in range(per_family):
            blob, label = fn(rng)
            name = f"{family}_{i:04d}.bin"
            with open(os.path.join(BAD_DIR, name), "wb") as fh:
                fh.write(blob)
            manifest.append((name, label, len(blob)))

    with open(os.path.join(HERE, "cases", "manifest.tsv"), "w") as fh:
        for name, label, size in manifest:
            fh.write(f"{name}\t{label}\t{size}\n")
    valid = sum(1 for m in manifest if m[1] == "valid")
    bad = len(manifest) - valid
    print(f"generated {valid} valid + {bad} malformed "
          f"({', '.join(MALFORMED)})")


if __name__ == "__main__":
    main()
