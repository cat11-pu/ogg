/*
 * decode_robust: fixed decoder.
 *
 * Recovery contract:
 *   1. ogg_sync_pageseek() < 0 (the same recapture signal that
 *      ogg_sync_pageout() reports as -1) is a *recoverable hint*, not a
 *      fatal error. Keep looping; the sync layer slides byte-by-byte to the
 *      next valid "OggS" capture header and only emits CRC-valid pages.
 *      pageseek is used directly so every skipped hop is counted.
 *   2. Each recapture means one or more unrecoverable pages were skipped;
 *      count lost pages using the Ogg page sequence number (header bytes
 *      18..21) delta, independent of how many byte-hops the search took.
 *   3. ogg_stream_packetout() == -1 is the stream layer's "hole" marker;
 *      consume it and continue so packet numbering stays aligned.
 *   4. Every recovered page is re-checked: Ogg CRC is already validated by
 *      the sync layer, and each payload frame has its own CRC-32 plus an
 *      application sequence number used to prove ordering after recovery.
 *
 * Usage: decode_robust <in.ogg> [--json] [--chunksize N]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ogg/ogg.h>
#include "repro_util.h"

typedef struct {
  long bytes_read;
  long resync_events;      /* lost-page recoveries (one per page gap) */
  long seek_hops;          /* negative returns from ogg_sync_pageseek() */
  long skipped_bytes;      /* bytes discarded while hunting capture headers */
  long pages_crc_ok;       /* pages emitted by the sync layer (CRC valid) */
  long pages_lost;         /* page-sequence gap size (bad pages skipped) */
  long packet_holes;       /* ogg_stream_packetout() returned -1 */
  long packets_ok;
  long payload_bad;        /* recovered frame failed app-level CRC */
  long first_lost_pageno;  /* -1 if nothing was lost */
  long prev_pageno;
  long first_gap;          /* first observed page sequence delta > 1 */
  long last_gap;
  int last_pktno;
  int order_ok;
  int checksum_ok;
} stats_t;

static void print_human(const stats_t *s) {
  printf("---------------- recovery statistics ----------------\n");
  printf("bytes read                 : %ld\n", s->bytes_read);
  printf("page recaptures (bad pages): %ld\n", s->resync_events);
  printf("page-seek skip hops        : %ld\n", s->seek_hops);
  printf("bytes skipped resyncing    : %ld\n", s->skipped_bytes);
  printf("pages passed Ogg CRC check : %ld\n", s->pages_crc_ok);
  printf("pages lost (bad, skipped)  : %ld\n", s->pages_lost);
  printf("packet-hole markers        : %ld\n", s->packet_holes);
  printf("payload frames recovered   : %ld\n", s->packets_ok);
  printf("payload CRC failures       : %ld\n", s->payload_bad);
  printf("first lost page sequence   : %ld\n", s->first_lost_pageno);
  printf("page-sequence gap (first)  : %ld\n", s->first_gap);
  printf("page-sequence gap (last)   : %ld\n", s->last_gap);
  printf("post-recovery page order   : %s\n", s->order_ok ? "OK" : "BROKEN");
  printf("post-recovery checksums    : %s\n",
         s->checksum_ok ? "OK" : "BROKEN");
  printf("-----------------------------------------------------\n");
}

static void print_json(const stats_t *s) {
  printf("{\n");
  printf("  \"bytes_read\": %ld,\n", s->bytes_read);
  printf("  \"resync_events\": %ld,\n", s->resync_events);
  printf("  \"seek_hops\": %ld,\n", s->seek_hops);
  printf("  \"skipped_bytes\": %ld,\n", s->skipped_bytes);
  printf("  \"pages_crc_ok\": %ld,\n", s->pages_crc_ok);
  printf("  \"pages_lost\": %ld,\n", s->pages_lost);
  printf("  \"packet_holes\": %ld,\n", s->packet_holes);
  printf("  \"packets_ok\": %ld,\n", s->packets_ok);
  printf("  \"payload_bad\": %ld,\n", s->payload_bad);
  printf("  \"first_lost_pageno\": %ld,\n", s->first_lost_pageno);
  printf("  \"first_gap\": %ld,\n", s->first_gap);
  printf("  \"last_gap\": %ld,\n", s->last_gap);
  printf("  \"order_ok\": %s,\n", s->order_ok ? "true" : "false");
  printf("  \"checksum_ok\": %s\n", s->checksum_ok ? "true" : "false");
  printf("}\n");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <in.ogg> [--json] [--chunksize N]\n",
            argv[0]);
    return 2;
  }
  int json = 0;
  long chunksize = 4096;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--json"))
      json = 1;
    else if (!strcmp(argv[i], "--chunksize") && i + 1 < argc)
      chunksize = strtol(argv[++i], NULL, 10);
  }
  if (chunksize < 1 || chunksize > 4096) {
    fprintf(stderr, "chunksize must be 1..4096\n");
    return 2;
  }

  FILE *in = fopen(argv[1], "rb");
  if (!in) {
    perror("open");
    return 1;
  }

  stats_t s;
  memset(&s, 0, sizeof(s));
  s.first_lost_pageno = -1;
  s.prev_pageno = -1;
  s.first_gap = -1;
  s.last_gap = -1;
  s.last_pktno = -1;
  s.order_ok = 1;
  s.checksum_ok = 1;

  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_sync_init(&oy);
  int stream_ready = 0;
  unsigned char chunk[4096];

  for (;;) {
    size_t got = fread(chunk, 1, (size_t)chunksize, in);
    s.bytes_read += (long)got;
    if (got > 0) {
      char *buf = ogg_sync_buffer(&oy, (long)got);
      memcpy(buf, chunk, got);
      ogg_sync_wrote(&oy, (long)got);
    }

    int more = 1;
    while (more) {
      ogg_page og;
      /*
       * Use the lower-level ogg_sync_pageseek() directly instead of
       * ogg_sync_pageout(): pageseek reports *every* negative (skipped)
       * hop, so we can count the true number of discarded bytes; pageout
       * collapses all but the first hop into its internal loop.
       */
      long r = ogg_sync_pageseek(&oy, &og);

      if (r == 0) {
        more = 0; /* need more input */
      } else if (r < 0) {
        /* Recapture: corrupt bytes were discarded while searching for the
         * next valid "OggS" capture header. Recoverable; keep pumping. */
        s.seek_hops++;
        s.skipped_bytes += -r;
        fprintf(stderr,
                "[seek] discarded %ld bytes, hunting next page header\n", -r);
      } else {
        long pageno = (long)ogg_page_pageno(&og);

        /* Quantify the skipped pages via the Ogg page sequence number. */
        if (s.prev_pageno >= 0) {
          long delta = pageno - s.prev_pageno;
          if (delta != 1) {
            s.resync_events++;
            s.last_gap = delta;
            if (s.first_gap < 0) {
              s.first_gap = delta;
              s.first_lost_pageno = s.prev_pageno + 1;
            }
            s.pages_lost += delta - 1;
            fprintf(stderr,
                    "[gap] page seq %ld -> %ld (lost %ld page(s))\n",
                    s.prev_pageno, pageno, delta - 1);
          }
        }
        s.prev_pageno = pageno;

        /* The sync layer only returns CRC-valid pages; record that. */
        s.pages_crc_ok++;

        if (!stream_ready) {
          ogg_stream_init(&os, ogg_page_serialno(&og));
          stream_ready = 1;
        }
        if (ogg_stream_pagein(&os, &og) != 0) {
          /* Wrong serial etc.; skip without touching the bookkeeping. */
          continue;
        }

        ogg_packet op;
        for (;;) {
          int pr = ogg_stream_packetout(&os, &op);
          if (pr == 0) break;       /* no complete packet yet */
          if (pr < 0) {
            /* Stream-layer hole marker for the dropped page. */
            s.packet_holes++;
            continue;
          }
          int pktno = repro_check_payload(op.packet, op.bytes);
          if (pktno < 0) {
            s.payload_bad++;
            s.checksum_ok = 0;
          } else {
            s.packets_ok++;
            /*
             * Ordering check on recovered frames: indices must be strictly
             * increasing. A single jump by (pages_lost + 1) at the damage
             * point is expected; duplicates, backwards steps or shuffling
             * are not.
             */
            if (s.last_pktno >= 0 && pktno <= s.last_pktno)
              s.order_ok = 0;
            s.last_pktno = pktno;
          }
        }
      }
    }

    if (got == 0) break;
  }

  if (s.payload_bad != 0) s.checksum_ok = 0;

  if (json)
    print_json(&s);
  else
    print_human(&s);

  ogg_sync_clear(&oy);
  if (stream_ready) ogg_stream_clear(&os);
  fclose(in);
  return 0;
}
