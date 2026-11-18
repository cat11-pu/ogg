/* Randomized differential/robustness fuzzer.
 *
 * Generates streams of (possibly malformed) Ogg pages with random segment
 * tables, body sizes, flags, serials and page numbers, feeds every byte
 * through the public sync + stream pipeline, then drains packets.  Any
 * ASan/UBSan report or assertion aborts the process; normal error codes
 * are counted instead.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ogg/ogg.h>

static unsigned long rng_state = 0x12345678UL;
static unsigned long xr(void) {
  unsigned long x = rng_state;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return (rng_state = x);
}
static unsigned rng(unsigned n) { return (unsigned)(xr() % n); }

static unsigned long crc_table[256];
static void crc_init(void) {
  for (unsigned i = 0; i < 256; i++) {
    unsigned long r = i << 24;
    for (int j = 0; j < 8; j++)
      r = (r & 0x80000000UL) ? ((r << 1) ^ 0x04c11db7UL) : (r << 1);
    crc_table[i] = r & 0xffffffffUL;
  }
}
static unsigned long crc(const unsigned char *p, long n) {
  unsigned long c = 0;
  for (long i = 0; i < n; i++)
    c = ((c << 8) ^ crc_table[((c >> 24) ^ p[i]) & 0xff]) & 0xffffffffUL;
  return c;
}

/* build one random page into buf; return total length, -1 on overflow */
static long make_page(unsigned char *buf, long cap, int serial) {
  int nseg = (int)(rng(256));
  long hlen = 27 + nseg;
  if (hlen + 1 > cap) return -1;
  memset(buf, 0, (size_t)hlen);
  memcpy(buf, "OggS", 4);
  buf[4] = (unsigned char)rng(3);            /* version: mostly 0,1,2 */
  buf[5] = (unsigned char)rng(8);            /* random flags */
  buf[26] = (unsigned char)nseg;
  long declared = 0;
  for (int i = 0; i < nseg; i++) {
    unsigned char s = (unsigned char)rng(256);
    buf[27 + i] = s;
    declared += s;
  }
  /* sometimes truncate or pad the body relative to the segment table */
  long body;
  switch (rng(4)) {
    case 0: body = declared; break;                 /* consistent */
    case 1: body = declared ? (long)rng((unsigned)declared + 1) : 0; break;
    case 2: body = declared + (long)rng(64); break;
    default: body = (long)rng(512); break;
  }
  if (hlen + body > cap) body = cap - hlen;
  for (long i = 0; i < body; i++) buf[hlen + i] = (unsigned char)rng(256);
  unsigned serialno = (rng(5) == 0)
                       ? (unsigned)rng(0xffffffffu) : (unsigned)serial;
  buf[14] = serialno & 0xff; buf[15] = (serialno >> 8) & 0xff;
  buf[16] = (serialno >> 16) & 0xff; buf[17] = (serialno >> 24) & 0xff;
  unsigned pageno = (unsigned)rng(16);              /* often out of order */
  buf[18] = pageno & 0xff; buf[19] = (pageno >> 8) & 0xff;
  buf[20] = (pageno >> 16) & 0xff; buf[21] = (pageno >> 24) & 0xff;
  if (rng(4) != 0) {                                /* mostly valid CRC */
    unsigned long c = crc(buf, hlen + body);
    buf[22] = c & 0xff; buf[23] = (c >> 8) & 0xff;
    buf[24] = (c >> 16) & 0xff; buf[25] = (c >> 24) & 0xff;
  }
  return hlen + body;
}

int main(int argc, char **argv) {
  long iterations = argc > 1 ? strtol(argv[1], NULL, 10) : 3000;
  if (iterations < 1) iterations = 1;
  rng_state = argc > 2 ? strtoul(argv[2], NULL, 10) : 0x12345678UL;
  crc_init();

  enum { CAP = 90000 };
  unsigned char *blob = malloc(CAP);
  long pages = 0, rejects = 0, accepted = 0, packets = 0, holes = 0;

  for (long it = 0; it < iterations; it++) {
    ogg_sync_state oy;
    ogg_stream_state os;
    ogg_sync_init(&oy);
    ogg_stream_init(&os, 42);

    long total = 0;
    int npages = 1 + (int)rng(6);
    for (int p = 0; p < npages; p++) {
      long n = make_page(blob + total, CAP - total, 42);
      if (n < 0) break;
      total += n;
    }
    /* occasionally corrupt random bytes */
    for (long c = 0; c < (long)rng(8); c++)
      if (total) blob[rng((unsigned)total)] ^= (unsigned char)(1u << rng(8));

    long pos = 0;
    while (pos < total) {
      long take = 1 + (long)rng(257);
      if (take > total - pos) take = total - pos;
      char *slot = ogg_sync_buffer(&oy, take);
      if (!slot) break;
      memcpy(slot, blob + pos, (size_t)take);
      if (ogg_sync_wrote(&oy, take) < 0) break;
      pos += take;

      for (;;) {
        ogg_page og;
        int s = ogg_sync_pageout(&oy, &og);
        if (s == 0) break;
        if (s < 0) { holes++; continue; }
        pages++;
        if (ogg_stream_pagein(&os, &og) < 0) rejects++;
        else accepted++;
        ogg_packet op;
        while (ogg_stream_packetout(&os, &op) > 0) {
          packets++;
          if (op.bytes > 0 && op.packet)
            for (long i = 0; i < op.bytes; i++) (void)op.packet[i];
        }
      }
    }

    /* also exercise raw pagein on the same blob to bypass CRC gate */
    long off = 0;
    while (off + 27 <= total) {
      if (memcmp(blob + off, "OggS", 4)) { off++; continue; }
      int nseg = blob[off + 26];
      long hlen = 27 + nseg;
      if (off + hlen > total) break;
      long blen = 0;
      for (int i = 0; i < nseg; i++) blen += blob[off + 27 + i];
      if (blen > total - off - hlen) blen = total - off - hlen;
      ogg_page og = { blob + off, hlen, blob + off + hlen, blen };
      if (ogg_stream_pagein(&os, &og) < 0) rejects++;
      else accepted++;
      ogg_packet op;
      while (ogg_stream_packetout(&os, &op) > 0) {
        packets++;
        if (op.bytes > 0 && op.packet)
          for (long i = 0; i < op.bytes; i++) (void)op.packet[i];
      }
      off += hlen + blen;
    }

    ogg_stream_clear(&os);
    ogg_sync_clear(&oy);
  }

  printf("iterations=%ld pages=%ld accepted=%ld rejected=%ld "
         "packets=%ld holes=%ld\n",
         iterations, pages, accepted, rejects, packets, holes);
  free(blob);
  return 0;
}
