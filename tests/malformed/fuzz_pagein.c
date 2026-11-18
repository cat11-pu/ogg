/* Fuzzing/regression harness for malformed Ogg pages.
 *
 * Feeds an input file through the public sync + stream decode pipeline and
 * exercises every parser entry point on the bytes.  A correct build must
 * never crash, read/write out of bounds, or trip UBsan; malformed pages must
 * be rejected with an error code and a byte offset instead.
 *
 * Usage: fuzz_pagein [--raw] <file>
 *   (default) sync-layer driven stream decode
 *   --raw     treat the whole file as one raw ogg_page handed to
 *             ogg_stream_pagein() (bypasses CRC validation)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ogg/ogg.h>

static unsigned char *read_file(const char *path, long *out_len) {
  FILE *fh = fopen(path, "rb");
  if (!fh) return NULL;
  fseek(fh, 0, SEEK_END);
  long len = ftell(fh);
  fseek(fh, 0, SEEK_SET);
  unsigned char *buf = malloc((size_t)len + 1);
  if (!buf) { fclose(fh); return NULL; }
  if (fread(buf, 1, (size_t)len, fh) != (size_t)len) {
    free(buf); fclose(fh); return NULL;
  }
  fclose(fh);
  *out_len = len;
  return buf;
}

static int run_raw(unsigned char *data, long len) {
  ogg_stream_state os;
  if (ogg_stream_init(&os, 0x04030201)) return 2;

  /* split the blob into raw pages using the on-disk header layout, bypassing
     the sync/CRC layer; each page is handed straight to ogg_stream_pagein */
  long off = 0;
  int index = 0;
  while (off + 27 <= len) {
    if (memcmp(data + off, "OggS", 4)) { off++; continue; }
    int nseg = data[off + 26];
    long hlen = 27 + nseg;
    if (off + hlen > len) break;
    long blen = 0;
    for (int i = 0; i < nseg; i++) blen += data[off + 27 + i];
    if (off + hlen + blen > len) {
      /* truncated page: still hand over what remains to prove no crash */
      blen = len - off - hlen;
    }

    ogg_page og;
    memset(&og, 0, sizeof(og));
    og.header = data + off;
    og.header_len = hlen;
    og.body = data + off + hlen;
    og.body_len = blen;

    int rc = ogg_stream_pagein(&os, &og);
    printf("raw page[%d] off=%ld rc=%d errcode=%d erroff=%ld\n",
           index, off, rc, ogg_stream_errcode(&os), ogg_stream_erroff(&os));

    ogg_packet op;
    memset(&op, 0, sizeof(op));
    while (ogg_stream_packetout(&os, &op) > 0) { }

    off += hlen + blen;
    index++;
  }

  ogg_stream_clear(&os);
  return 0;
}

int main(int argc, char **argv) {
  int raw = 0;
  const char *path = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--raw")) raw = 1;
    else path = argv[i];
  }
  if (!path) { fprintf(stderr, "usage: %s [--raw] file\n", argv[0]); return 2; }

  long len;
  unsigned char *data = read_file(path, &len);
  if (!data) { perror(path); return 2; }

  if (raw) {
    int rc = run_raw(data, len);
    free(data);
    return rc;
  }

  ogg_sync_state oy;
  ogg_stream_state os;
  if (ogg_sync_init(&oy)) return 2;
  if (ogg_stream_init(&os, 0x04030201)) return 2;

  /* feed in varying chunk sizes to exercise partial-page buffering */
  long pos = 0;
  long page_count = 0, reject_count = 0, hole_count = 0;
  static const long chunks[] = {1, 2, 3, 7, 16, 33, 64, 128, 257, 4096};
  int ci = 0;

  while (pos < len) {
    long want = chunks[ci++ % (int)(sizeof(chunks) / sizeof(chunks[0]))];
    long take = want < (len - pos) ? want : (len - pos);
    char *slot = ogg_sync_buffer(&oy, take);
    if (!slot) break;
    memcpy(slot, data + pos, (size_t)take);
    if (ogg_sync_wrote(&oy, take) < 0) break;
    pos += take;

    for (;;) {
      ogg_page og;
      memset(&og, 0, sizeof(og));
      int sret = ogg_sync_pageout(&oy, &og);
      if (sret == 0) break;
      if (sret == -1) { hole_count++; continue; }

      page_count++;
      long page_off = (long)(og.header - oy.data);
      int rc = ogg_stream_pagein(&os, &og);
      if (rc < 0) {
        reject_count++;
        printf("reject page#%ld stream_off=%ld rc=%d errcode=%d erroff=%ld\n",
               page_count, page_off, rc,
               ogg_stream_errcode(&os), ogg_stream_erroff(&os));
      }

      ogg_packet op;
      memset(&op, 0, sizeof(op));
      while (ogg_stream_packetout(&os, &op) > 0) { }
    }
  }

  printf("pages=%ld rejected=%ld holes=%ld\n",
         page_count, reject_count, hole_count);

  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);
  free(data);
  return 0;
}
