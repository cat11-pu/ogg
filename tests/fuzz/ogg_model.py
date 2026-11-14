"""Minimal, independent Ogg framing implementation (RFC 3533).

Used both to build bitstreams and as the reference parser that the
libogg fuzz driver output is diffed against.  Written from the RFC,
not copied from libogg, so agreement is a genuine cross-check.
"""

import struct

CRC_POLY = 0x04C11DB7


def _make_crc_table():
    table = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ CRC_POLY) & 0xFFFFFFFF if r & 0x80000000 else (r << 1)
        table.append(r)
    return table


CRC_TABLE = _make_crc_table()


def crc32ogg(buf: bytes) -> int:
    crc = 0
    for b in buf:
        crc = ((crc << 8) ^ CRC_TABLE[((crc >> 24) & 0xFF) ^ b]) & 0xFFFFFFFF
    return crc


CONTINUED = 0x01
BOS = 0x02
EOS = 0x04


def split_lacing(payload_len: int):
    """Split one packet body into lacing segment sizes (each <= 255)."""
    full, rem = divmod(payload_len, 255)
    segments = [255] * full
    if rem or not segments:
        segments.append(rem)
    return segments


class PageBuilder:
    """Accumulates packets and emits Ogg pages like ogg_stream_flush."""

    def __init__(self, serial: int):
        self.serial = serial
        self.pageno = 0
        self._pending = bytearray()  # packet data not yet paged

    def _build_page(self, seg_table, body, flags, granule):
        nseg = len(seg_table)
        assert nseg <= 255
        header = bytearray(27)
        header[0:4] = b"OggS"
        header[4] = 0  # version
        header[5] = flags
        struct.pack_into("<q", header, 6, granule & 0xFFFFFFFFFFFFFFFF)
        struct.pack_into("<I", header, 14, serial_u32(self.serial))
        struct.pack_into("<I", header, 18, self.pageno & 0xFFFFFFFF)
        header[22:26] = b"\x00\x00\x00\x00"
        header[26] = nseg
        page = bytes(header) + bytes(seg_table) + bytes(body)
        crc = crc32ogg(page)
        page = page[:22] + struct.pack("<I", crc) + page[26:]
        self.pageno += 1
        return page

    def flush_packet(self, payload: bytes, flags: int = 0, granule: int = -1):
        """Force a packet (or its first page for huge packets) into a page."""
        pages = []
        data = self._pending + payload
        self._pending = bytearray()
        seg = split_lacing(len(data))
        if len(seg) <= 255:
            body = data
            pages.append(self._build_page(seg, body, flags,
                                          granule if granule >= 0 else 0))
            return pages
        # Packet spans multiple pages: 255 segments of 255 = 65025 per page.
        offset = 0
        first = True
        while len(seg) > 255:
            page_seg = seg[:255]
            page_body = data[offset:offset + 65025]
            offset += 65025
            f = CONTINUED if not first else (flags & BOS)
            pages.append(self._build_page(page_seg, page_body, f, 0))
            first = False
            seg = seg[255:]
        page_body = data[offset:]
        f = CONTINUED
        pages.append(self._build_page(seg, page_body, f,
                                      granule if granule >= 0 else 0))
        return pages

    def pack(self, packets, max_segments=255):
        """Pack several packets, splitting pages at max_segments."""
        work = list(packets)  # list of (payload, flags, granule)
        pages = []
        seg_table = []
        body = bytearray()
        flags_acc = 0
        granule_last = 0
        idx = 0
        while idx < len(work):
            payload, flags, granule = work[idx]
            segs = split_lacing(len(payload))
            if len(seg_table) + len(segs) > max_segments and seg_table:
                pages.append(self._build_page(seg_table, body, flags_acc,
                                              granule_last))
                seg_table, body, flags_acc = [], bytearray(), 0
                continue
            if len(segs) > 255:
                # Huge packet: flush current page, then dedicated pages.
                if seg_table:
                    pages.append(self._build_page(seg_table, body, flags_acc,
                                                  granule_last))
                    seg_table, body, flags_acc = [], bytearray(), 0
                sub = self.flush_packet(payload, flags, granule)
                pages.extend(sub)
                idx += 1
                continue
            seg_table.extend(segs)
            body.extend(payload)
            flags_acc |= flags
            if granule >= 0:
                granule_last = granule
            idx += 1
            if len(seg_table) >= max_segments:
                pages.append(self._build_page(seg_table, body, flags_acc,
                                              granule_last))
                seg_table, body, flags_acc = [], bytearray(), 0
        if seg_table:
            pages.append(self._build_page(seg_table, body, flags_acc,
                                          granule_last))
        return pages


def serial_u32(v: int) -> int:
    return v & 0xFFFFFFFF


def pageno_u32(v: int) -> int:
    return v & 0xFFFFFFFF


class RefPage:
    def __init__(self, offset, header, body):
        self.offset = offset
        self.header = header
        self.body = body
        self.version = header[4]
        self.flags = header[5]
        self.granule = struct.unpack_from("<q", header, 6)[0]
        self.serial = struct.unpack_from("<I", header, 14)[0]
        self.pageno = struct.unpack_from("<I", header, 18)[0]
        self.stored_crc = struct.unpack_from("<I", header, 22)[0]
        self.nseg = header[26]
        self.seg_table = list(header[27:27 + self.nseg])
        self.body_len = sum(self.seg_table)

    @property
    def total_len(self):
        return 27 + self.nseg + self.body_len

    def crc_ok(self):
        page = self.header[:22] + b"\x00\x00\x00\x00" + self.header[26:] + self.body
        return crc32ogg(page) == self.stored_crc


def find_pages(data: bytes):
    """RFC-level scan: every capture-pattern candidate, classified."""
    out = []
    pos = 0
    n = len(data)
    while pos < n:
        idx = data.find(b"OggS", pos)
        if idx < 0:
            break
        if idx + 27 > n:
            out.append(("truncated_header", idx, None))
            break
        nseg = data[idx + 26]
        header_len = 27 + nseg
        if idx + header_len > n:
            out.append(("truncated_header", idx, None))
            pos = idx + 1
            continue
        body_len = sum(data[idx + 27 + i] for i in range(nseg))
        total = header_len + body_len
        if idx + total > n:
            out.append(("page_length_mismatch", idx, None))
            pos = idx + 1
            continue
        header = data[idx:idx + header_len]
        body = data[idx + header_len:idx + total]
        out.append(("page", idx, RefPage(idx, header, body)))
        pos = idx + 1
    # Incomplete capture pattern left in the trailing bytes.
    tail = data.find(b"O", pos)
    if tail >= 0 and data[tail:tail + 4] != b"OggS":
        prefix = data[tail:]
        if b"OggS".startswith(prefix) and len(prefix) < 4:
            out.append(("truncated_header", tail, None))
    return out


class RefStream:
    """RFC 3533 packet reassembly for one logical bitstream.

    A packet spans lacing segments: zero or more 255-byte segments
    terminated by a segment < 255 (the terminator gives the final
    length, possibly 0).  Continuation pages extend an open packet.
    Out-of-sequence page numbers drop any open partial packet, the
    same recovery rule libogg implements.
    """

    def __init__(self, serial):
        self.serial = serial
        self.pageno = 0
        self.open = bytearray()
        self.open_bos = 0
        self.eos = False
        self.started = False

    def pagein(self, page: RefPage):
        """Feed one checksum-valid page; return completed packet dicts."""
        if page.serial != self.serial or page.version != 0:
            return ["REJECT"]
        packets = []
        gap = (self.started and
               pageno_u32(page.pageno) != pageno_u32(self.pageno))
        if gap:
            self.open = bytearray()
            self.open_bos = 0

        continued = bool(page.flags & CONTINUED)
        bos = bool(page.flags & BOS)
        eos = bool(page.flags & EOS)
        seg = list(page.seg_table)
        body = page.body
        pos = 0
        i = 0
        if continued and (gap or not self.open):
            # Continuation with nothing to continue: skip the first
            # partial packet exactly like libogg's continued-page rule.
            while i < len(seg):
                val = seg[i]
                pos += val
                i += 1
                if val < 255:
                    break

        first_packet = True
        while i < len(seg):
            val = seg[i]
            self.open.extend(body[pos:pos + val])
            pos += val
            i += 1
            if first_packet and bos and not self.open_bos:
                self.open_bos = 1
            if val < 255:
                pkt_bos = self.open_bos
                pkt_eos = 1 if (eos and i == len(seg)) else 0
                packets.append({
                    "serial": self.serial,
                    "bytes": len(self.open),
                    "b_o_s": pkt_bos,
                    "e_o_s": pkt_eos,
                    "granule": page.granule if pkt_eos else 0,
                    "hash": hash_bytes(bytes(self.open)),
                })
                self.open = bytearray()
                self.open_bos = 0
                first_packet = False

        if eos:
            self.eos = True
        self.pageno = (page.pageno + 1) & 0xFFFFFFFF
        self.started = True
        return packets


def reference_decode(data: bytes):
    """Full reference pass over a byte stream.

    Returns (page_events, packet_events, diag_events) using the same
    sync/reassembly rules as the libogg driver so outputs can diff.
    """
    diag = []
    pages = []
    for kind, offset, page in find_pages(data):
        if kind != "page":
            reason = kind
            diag.append({"offset": offset, "reason": reason})
            continue
        problems = []
        if page.version != 0:
            problems.append("illegal_version")
        if page.flags & ~0x07:
            problems.append("illegal_flags")
        if not page.crc_ok():
            problems.append("checksum_mismatch")
        if problems:
            for reason in problems:
                diag.append({"offset": offset, "reason": reason,
                             "serial": page.serial, "pageno": page.pageno})
            continue
        pages.append(page)
        diag.append({"offset": offset, "reason": None,
                     "serial": page.serial, "pageno": page.pageno,
                     "version": page.version, "flags": page.flags,
                     "granule": page.granule, "nseg": page.nseg,
                     "body_len": page.body_len,
                     "total": page.total_len})

    streams = {}
    packet_events = []
    for page in pages:
        st = streams.get(page.serial)
        if st is None:
            st = RefStream(page.serial)
            streams[page.serial] = st
        for pkt in st.pagein(page):
            if pkt == "REJECT":
                continue
            packet_events.append(pkt)
    return pages, packet_events, diag


def hash_bytes(b: bytes) -> int:
    h = 2166136261
    for x in b:
        h ^= x
        h = (h * 16777619) & 0xFFFFFFFF
    return h
