/*
 * corrupt_stream: damage exactly one Ogg page in a bitstream while keeping the
 * total file length unchanged, so subsequent pages keep their original
 * offsets. The damaged page fails the Ogg page CRC and must be skipped; every
 * later page remains intact and independently checksum-valid.
 *
 * Modes:
 *   body   (default) flip bytes inside the page body; the "OggS" header stays
 *          valid, which models payload/segment corruption.
 *   header overwrite the capture pattern, modeling a destroyed page header.
 *
 * Usage: corrupt_stream <in.ogg> <out.ogg> <page_index> [body|header]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "repro_util.h"

static unsigned char *read_all(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "open %s: %s\n", path, strerror(errno));
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *buf = malloc((size_t)n + 1);
  if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fprintf(stderr, "read %s failed\n", path);
    free(buf);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *len = (size_t)n;
  return buf;
}

/* Locate the [start,end) byte range of page page_idx using page headers. */
static int find_page(const unsigned char *buf, size_t len,
                     unsigned want_idx, size_t *start, size_t *end) {
  size_t pos = 0;
  unsigned idx = 0;
  while (pos + OGG_HEADER_FIXED <= len) {
    if (memcmp(buf + pos, OGG_CAPTURE_PATTERN, 4) != 0) {
      fprintf(stderr, "expected capture pattern at offset %zu\n", pos);
      return -1;
    }
    int nseg = buf[pos + 26];
    if (pos + OGG_HEADER_FIXED + (size_t)nseg > len) return -1;
    long body = 0;
    for (int i = 0; i < nseg; i++) body += buf[pos + 27 + i];
    size_t next = pos + OGG_HEADER_FIXED + (size_t)nseg + (size_t)body;
    if (next > len) return -1;
    if (idx == want_idx) {
      *start = pos;
      *end = next;
      return 0;
    }
    idx++;
    pos = next;
  }
  return -1;
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr,
            "usage: %s <in.ogg> <out.ogg> <page_index> [body|header]\n",
            argv[0]);
    return 2;
  }
  const char *mode = argc >= 5 ? argv[4] : "body";
  if (strcmp(mode, "body") && strcmp(mode, "header")) {
    fprintf(stderr, "mode must be 'body' or 'header'\n");
    return 2;
  }

  size_t len;
  unsigned char *buf = read_all(argv[1], &len);
  if (!buf) return 1;

  unsigned target = (unsigned)strtoul(argv[3], NULL, 10);
  size_t start, end;
  if (find_page(buf, len, target, &start, &end) != 0) {
    fprintf(stderr, "page %u not found\n", target);
    return 1;
  }

  int nseg = buf[start + 26];
  size_t header_len = OGG_HEADER_FIXED + (size_t)nseg;
  size_t body_len = end - start - header_len;

  if (!strcmp(mode, "header")) {
    /* Destroy the capture pattern; page length fields are untouched. */
    buf[start + 0] = 'X';
    buf[start + 1] = 'X';
    buf[start + 2] = 'X';
    buf[start + 3] = 'X';
  } else {
    /*
     * Deterministically flip 16 spread-out bytes in the body. The page CRC
     * then mismatches even though the OggS header still looks like a page.
     */
    unsigned seed = 0x9e3779b9u;
    for (int k = 0; k < 16 && (size_t)k < body_len; k++) {
      seed = seed * 1664525u + 1013904223u;
      size_t off = start + header_len + (seed % body_len);
      buf[off] = (unsigned char)(buf[off] ^ (unsigned char)(0x80u | (k + 1)));
    }
  }

  FILE *out = fopen(argv[2], "wb");
  if (!out || fwrite(buf, 1, len, out) != len) {
    fprintf(stderr, "write %s: %s\n", argv[2], strerror(errno));
    return 1;
  }
  fclose(out);
  free(buf);

  printf("corrupted page %u (%s mode), page bytes [%zu,%zu), "
         "file length unchanged (%zu)\n",
         target, mode, start, end, len);
  return 0;
}
