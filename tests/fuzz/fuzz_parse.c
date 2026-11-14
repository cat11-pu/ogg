/*
 * Ogg parser fuzzing driver.
 *
 * Feeds an arbitrary byte buffer (read from a file, or stdin when the
 * path is "-") through the full public decode path:
 *   ogg_sync_*  -> ogg_sync_pageout -> ogg_stream_pagein
 *               -> ogg_stream_packetout
 *
 * Every observed page/packet event is emitted as one machine-readable
 * line so an independent reference implementation can be diffed against
 * it.  Before handing each page to the stream layer, a standalone
 * diagnostic pass classifies malformed pages with byte offsets and
 * reasons (the library API itself only returns pass/fail codes).
 *
 * Build: see build.sh (ASan + UBSan + LSan).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ogg/ogg.h>

/* CRC-32 per RFC 3533, polynomial 0x04c11db7, init/xorout = 0. */
static uint32_t crc_table[256];

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i << 24;
        for (int j = 0; j < 8; j++)
            r = (r & 0x80000000U) ? (r << 1) ^ 0x04c11db7U : (r << 1);
        crc_table[i] = r;
    }
}

static uint32_t crc_compute(const unsigned char *buf, size_t len) {
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++)
        crc = (crc << 8) ^ crc_table[((crc >> 24) & 0xff) ^ buf[i]];
    return crc;
}

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/*
 * Diagnostic scan of the whole byte stream.  Every "OggS" capture is
 * inspected and classified, independently of the library's own state
 * machine, so failures can be reported with exact offsets/reasons.
 */
static void diagnose(const unsigned char *data, size_t len) {
    size_t off = 0;
    static const unsigned char capture[4] = {'O','g','g','S'};
    while (off + 4 <= len) {
        const unsigned char *p = memchr(data + off, 'O', len - off);
        if (!p) break;
        size_t pos = (size_t)(p - data);
        if (pos + 27 > len) {
            printf("DIAG offset=%zu reason=truncated_header detail='capture pattern at end of stream, only %zu bytes remain'\n",
                   pos, len - pos);
            break;
        }
        if (memcmp(p, "OggS", 4) != 0) { off = pos + 1; continue; }

        unsigned char version = p[4];
        unsigned char hflags = p[5];
        uint64_t granule = rd64(p + 6);
        uint32_t serial = rd32(p + 14);
        uint32_t pageno = rd32(p + 18);
        uint32_t stored_crc = rd32(p + 22);
        int nseg = p[26];
        long header_len = 27 + nseg;
        if ((size_t)header_len > len - pos) {
            printf("DIAG offset=%zu serial=%u pageno=%u reason=truncated_header detail='segment table declares %d entries, %zu bytes remain'\n",
                   pos, serial, pageno, nseg, len - pos);
            off = pos + 1;
            continue;
        }
        long body_len = 0;
        for (int i = 0; i < nseg; i++) body_len += p[27 + i];
        long total = header_len + body_len;

        if ((size_t)total > len - pos) {
            /* Declared page length (header + lacing-declared body) runs
               past the actual buffered data: page-length desync. */
            printf("DIAG offset=%zu serial=%u pageno=%u reason=page_length_mismatch detail='declared %ld bytes, %zu available'\n",
                   pos, serial, pageno, total, len - pos);
            off = pos + 1;
            continue;
        }
        if (version != 0) {
            printf("DIAG offset=%zu serial=%u pageno=%u reason=illegal_version detail='version=%u'\n",
                   pos, serial, pageno, version);
        }
        if (hflags & ~0x07U) {
            printf("DIAG offset=%zu serial=%u pageno=%u reason=illegal_flags detail='flags=0x%02x'\n",
                   pos, serial, pageno, hflags);
        }
        unsigned char tmp[4];
        memcpy(tmp, p + 22, 4);
        memset((unsigned char *)p + 22, 0, 4);
        uint32_t calc = crc_compute(p, (size_t)total);
        memcpy((unsigned char *)p + 22, tmp, 4);
        if (calc != stored_crc) {
            printf("DIAG offset=%zu serial=%u pageno=%u reason=checksum_mismatch detail='stored=0x%08x computed=0x%08x'\n",
                   pos, serial, pageno, stored_crc, calc);
        }
        printf("DIAG offset=%zu serial=%u pageno=%u granule=%llu flags=0x%02x nseg=%d body=%ld total=%ld verdict=page_structurally_valid\n",
               pos, serial, pageno, (unsigned long long)granule, hflags,
               nseg, body_len, total);
        off = pos + 1;
    }
    /* Partial capture pattern at the very end ("O", "Og", "Ogg"). */
    if (off < len) {
        const unsigned char *p = memchr(data + off, 'O', len - off);
        if (p) {
            size_t pos = (size_t)(p - data);
            size_t remain = len - pos;
            int prefix = 1;
            while (prefix < 4 && prefix < (int)remain &&
                   p[prefix] == capture[prefix])
                prefix++;
            if (prefix == (int)remain && remain < 4)
                printf("DIAG offset=%zu reason=truncated_header detail='incomplete capture pattern, %zu of 4 bytes'\n",
                       pos, remain);
        }
    }
}

/* FNV-1a so chunk sizes are deterministic per input file. */
static uint32_t fnv1a(const unsigned char *d, size_t n) {
    uint32_t h = 2166136261U;
    for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 16777619U; }
    return h;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file|-> [chunk-size]\n", argv[0]);
        return 2;
    }
    FILE *f = (!strcmp(argv[1], "-")) ? stdin : fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return 2; }
    fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)sz + 1);
    if (!data) { fclose(f); return 2; }
    size_t rd = fread(data, 1, (size_t)sz, f);
    fclose(f);

    crc_init();
    printf("INPUT bytes=%zu\n", rd);
    diagnose(data, rd);

    int fixed_chunk = (argc >= 3) ? atoi(argv[2]) : 0;

    ogg_sync_state oy;
    if (ogg_sync_init(&oy)) {
        fprintf(stderr, "ogg_sync_init failed\n");
        free(data);
        return 1;
    }

    /* One stream-state per serial number, exercising pagein across
       interleaved logical bitstreams and cyclic page numbers. */
    enum { MAX_SERIALS = 64 };
    ogg_stream_state *streams[MAX_SERIALS] = {0};
    int serials[MAX_SERIALS];
    long expected_pageno[MAX_SERIALS];
    int seen_pageno[MAX_SERIALS];
    int nstreams = 0;

    size_t pos = 0;
    int sync_holes = 0;
    size_t delivered = 0;   /* total bytes handed to the sync layer */
    while (pos < rd) {
        size_t chunk;
        if (fixed_chunk > 0)
            chunk = (size_t)fixed_chunk;
        else {
            /* Mostly realistic 4 KiB writes; every ~17th write is a
               1-byte boundary probe so page headers/bodies are still
               split across calls. Avoid tiny chunks on average since
               ogg_sync_buffer() memmoves retained data each call. */
            uint32_t h = fnv1a(data + pos,
                               rd - pos > 64 ? 64 : rd - pos);
            chunk = (h % 17 == 0) ? (size_t)(1 + h % 8)
                                   : (size_t)(2048 + h % 4096);
        }
        if (chunk > rd - pos) chunk = rd - pos;
        char *buf = ogg_sync_buffer(&oy, (long)chunk);
        if (!buf) {
            printf("SYNC offset=%zu reason=allocator_refused detail='ogg_sync_buffer returned NULL for %zu bytes'\n",
                   pos, chunk);
            break;
        }
        memcpy(buf, data + pos, chunk);
        if (ogg_sync_wrote(&oy, (long)chunk) != 0) {
            printf("SYNC offset=%zu reason=wrote_rejected detail='ogg_sync_wrote returned -1'\n", pos);
            break;
        }
        pos += chunk;
        delivered += chunk;

        for (;;) {
            ogg_page og;
            int r = ogg_sync_pageout(&oy, &og);
            if (r == 0) break;
            if (r == -1) {
                sync_holes++;
                printf("SYNC reason=recapture detail='hole/junk skipped before next capture' holes=%d\n",
                       sync_holes);
                continue;
            }
            /* og.header points into oy.data, whose base moves on
               reallocation, so derive the stream-absolute offset from
               sync-layer accounting.  After a successful pageseek,
               oy.returned already includes this page, hence its start
               within the delivered stream is returned - page_size. */
            long page_off = (long)(delivered - (oy.fill - oy.returned) -
                                   (og.header_len + og.body_len));
            int serial = ogg_page_serialno(&og);
            long pageno = ogg_page_pageno(&og);
            printf("PAGE offset=%ld serial=%d pageno=%ld version=%d flags=0x%02x granule=%lld header_len=%ld body_len=%ld continued=%d bos=%d eos=%d packets=%d\n",
                   page_off, serial, pageno, ogg_page_version(&og),
                   og.header[5], (long long)ogg_page_granulepos(&og),
                   og.header_len, og.body_len,
                   ogg_page_continued(&og), ogg_page_bos(&og),
                   ogg_page_eos(&og), ogg_page_packets(&og));

            int slot = -1;
            for (int i = 0; i < nstreams; i++)
                if (serials[i] == serial) { slot = i; break; }
            if (slot < 0) {
                if (nstreams < MAX_SERIALS) {
                    slot = nstreams++;
                    serials[slot] = serial;
                    streams[slot] = (ogg_stream_state *)malloc(
                        sizeof(ogg_stream_state));
                    if (!streams[slot] ||
                        ogg_stream_init(streams[slot], serial)) {
                        printf("STREAM offset=%ld serial=%d reason=init_failed\n",
                               page_off, serial);
                        free(streams[slot]);
                        streams[slot] = NULL;
                        nstreams--;
                        continue;
                    }
                    expected_pageno[slot] = 0;
                    seen_pageno[slot] = 0;
                } else {
                    printf("STREAM offset=%ld serial=%d reason=serial_table_full\n",
                           page_off, serial);
                    continue;
                }
            }

            /* Page-chain diagnostics: every page in a logical bitstream
               must carry a strictly incrementing (mod 2^32) sequence. */
            if (seen_pageno[slot]) {
                long want = expected_pageno[slot];
                if ((uint32_t)pageno == (uint32_t)(want - 1)) {
                    printf("DIAG offset=%ld serial=%d pageno=%ld reason=cyclic_page_chain detail='page number repeats, expected %ld'\n",
                           page_off, serial, pageno, want);
                } else if ((uint32_t)pageno != (uint32_t)want) {
                    printf("DIAG offset=%ld serial=%d pageno=%ld reason=cyclic_page_chain detail='non-monotonic sequence, expected %ld'\n",
                           page_off, serial, pageno, want);
                }
            }
            expected_pageno[slot] =
                (long)(((uint32_t)pageno) + 1U);
            seen_pageno[slot] = 1;

            int pr = ogg_stream_pagein(streams[slot], &og);
            if (pr != 0) {
                printf("STREAM offset=%ld serial=%d pageno=%ld reason=pagein_rejected detail='ogg_stream_pagein returned %d'\n",
                       page_off, serial, pageno, pr);
            } else {
                ogg_packet op;
                for (;;) {
                    int pr2 = ogg_stream_packetout(streams[slot], &op);
                    if (pr2 <= 0) break;
                    unsigned char d0 = op.bytes ? op.packet[0] : 0;
                    unsigned char dl = (op.bytes > 1) ? op.packet[op.bytes - 1] : 0;
                    printf("PACKET serial=%d bytes=%ld b_o_s=%d e_o_s=%d granule=%lld packetno=%lld first=0x%02x last=0x%02x\n",
                           serial, op.bytes, op.b_o_s ? 1 : 0, op.e_o_s ? 1 : 0,
                           (long long)op.granulepos, (long long)op.packetno,
                           d0, dl);
                    (void)dl;
                }
            }
        }
    }

    for (int i = 0; i < nstreams; i++) {
        printf("STREAM_CLEAR serial=%d pageno_next=%ld e_o_s=%d\n",
               serials[i], streams[i]->pageno, streams[i]->e_o_s);
        ogg_stream_clear(streams[i]);
        free(streams[i]);
    }
    ogg_sync_clear(&oy);
    free(data);
    printf("DONE status=clean holes=%d streams=%d\n", sync_holes, nstreams);
    return 0;
}
