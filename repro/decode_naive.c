/*
 * decode_naive: the defective reference decoder. It treats the first
 * ogg_sync_pageout() recapture report (return value -1) as a fatal stream
 * error and aborts, so one corrupt page causes every later page to be lost.
 *
 * Usage: decode_naive <in.ogg> [--chunksize N]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ogg/ogg.h>
#include "repro_util.h"

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <in.ogg> [--chunksize N]\n", argv[0]);
    return 2;
  }
  long chunksize = 4096;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--chunksize") && i + 1 < argc)
      chunksize = strtol(argv[++i], NULL, 10);
  }
  FILE *in = fopen(argv[1], "rb");
  if (!in) {
    perror("open");
    return 1;
  }

  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_sync_init(&oy);

  int stream_ready = 0;
  long pages_ok = 0, packets_ok = 0;
  unsigned char chunk[4096];

  for (;;) {
    size_t got = fread(chunk, 1, (size_t)chunksize, in);
    if (got > 0) {
      char *buf = ogg_sync_buffer(&oy, (long)got);
      memcpy(buf, chunk, got);
      ogg_sync_wrote(&oy, (long)got);
    }

    int more = 1;
    while (more) {
      ogg_page og;
      int r = ogg_sync_pageout(&oy, &og);
      if (r == 0) {
        more = 0; /* need more data */
      } else if (r < 0) {
        /* BUG: a recoverable recapture is handled as a fatal error. */
        fprintf(stderr,
                "NAIVE FATAL: sync lost (pageout returned -1); aborting "
                "after %ld valid pages, %ld valid packets\n",
                pages_ok, packets_ok);
        ogg_sync_clear(&oy);
        if (stream_ready) ogg_stream_clear(&os);
        fclose(in);
        return 3;
      } else {
        if (!stream_ready) {
          ogg_stream_init(&os, ogg_page_serialno(&og));
          stream_ready = 1;
        }
        if (ogg_stream_pagein(&os, &og) != 0)
          continue;
        pages_ok++;
        ogg_packet op;
        while (ogg_stream_packetout(&os, &op) == 1) packets_ok++;
      }
    }

    if (got == 0) break;
  }

  printf("NAIVE: decoded %ld pages, %ld packets (no corruption seen)\n",
         pages_ok, packets_ok);
  ogg_sync_clear(&oy);
  if (stream_ready) ogg_stream_clear(&os);
  fclose(in);
  return 0;
}
