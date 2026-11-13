/*
 * gen_stream: build a clean, page-structured Ogg bitstream for recovery
 * testing. One packet (one payload frame) is flushed per page so that losing a
 * single page corresponds to losing exactly one packet.
 *
 * Usage: gen_stream <out.ogg> [npages]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <ogg/ogg.h>
#include "repro_util.h"

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <out.ogg> [npages]\n", argv[0]);
    return 2;
  }
  unsigned npages = argc >= 3 ? (unsigned)strtoul(argv[2], NULL, 10)
                              : REPRO_DEFAULT_PAGES;
  if (npages < 3 || npages > 100000) {
    fprintf(stderr, "npages out of range\n");
    return 2;
  }

  ogg_stream_state os;
  if (ogg_stream_init(&os, (int)REPRO_SERIALNO) != 0) {
    fprintf(stderr, "ogg_stream_init failed\n");
    return 1;
  }

  FILE *out = fopen(argv[1], "wb");
  if (!out) {
    fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
    return 1;
  }

  unsigned char payload[REPRO_PAYLOAD_SIZE];
  for (unsigned i = 0; i < npages; i++) {
    repro_make_payload(payload, (uint16_t)i);

    ogg_packet op;
    memset(&op, 0, sizeof(op));
    op.packet = payload;
    op.bytes = REPRO_PAYLOAD_SIZE;
    op.b_o_s = (i == 0);
    op.e_o_s = (i == npages - 1);
    op.granulepos = (ogg_int64_t)(i + 1);
    op.packetno = i;

    if (ogg_stream_packetin(&os, &op) != 0) {
      fprintf(stderr, "packetin %u failed\n", i);
      return 1;
    }

    ogg_page og;
    /* flush forces a one-packet-per-page layout with a fresh capture header */
    while (ogg_stream_flush(&os, &og)) {
      if (fwrite(og.header, 1, og.header_len, out) != (size_t)og.header_len ||
          fwrite(og.body, 1, og.body_len, out) != (size_t)og.body_len) {
        fprintf(stderr, "write failed\n");
        return 1;
      }
    }
  }

  fclose(out);
  ogg_stream_clear(&os);
  printf("generated %u pages -> %s\n", npages, argv[1]);
  return 0;
}
