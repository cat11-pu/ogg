/* Byte-level regression: decode the valid reference stream and print every
 * packet's metadata and a hash of its bytes.  The output before and after
 * the hardening change must be identical for all well-formed input.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ogg/ogg.h>

#ifndef OGG_ERR_NONE
#define ogg_stream_errcode(os) (0)
#endif

static unsigned long djb2(const unsigned char *p, long n) {
  unsigned long h = 5381UL;
  for (long i = 0; i < n; i++) h = ((h << 5) + h) + p[i];
  return h;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s file.ogg\n", argv[0]); return 2; }
  FILE *fh = fopen(argv[1], "rb");
  if (!fh) { perror(argv[1]); return 2; }
  fseek(fh, 0, SEEK_END);
  long len = ftell(fh);
  fseek(fh, 0, SEEK_SET);
  unsigned char *data = malloc((size_t)len);
  if (fread(data, 1, (size_t)len, fh) != (size_t)len) return 2;
  fclose(fh);

  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_sync_init(&oy);
  ogg_stream_init(&os, 0x04030201);

  long pos = 0, npackets = 0;
  while (pos < len) {
    long take = len - pos;
    if (take > 128) take = 128;
    char *slot = ogg_sync_buffer(&oy, take);
    memcpy(slot, data + pos, (size_t)take);
    ogg_sync_wrote(&oy, take);
    pos += take;

    ogg_page og;
    while (ogg_sync_pageout(&oy, &og) == 1) {
      if (ogg_stream_pagein(&os, &og) < 0) {
        printf("UNEXPECTED REJECT errcode=%d\n", ogg_stream_errcode(&os));
        return 1;
      }
      ogg_packet op;
      while (ogg_stream_packetout(&os, &op) == 1) {
        npackets++;
        printf("packet no=%lld bos=%ld eos=%ld gran=%lld bytes=%ld "
               "hash=%08lx\n",
               (long long)op.packetno, op.b_o_s, op.e_o_s,
               (long long)op.granulepos, op.bytes,
               djb2(op.packet, op.bytes));
      }
    }
  }

  printf("packets=%ld\n", npackets);
  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);
  free(data);
  return npackets == 3 ? 0 : 1;
}
