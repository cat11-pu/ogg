#ifndef REPRO_UTIL_H
#define REPRO_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define OGG_CAPTURE_PATTERN "OggS"
#define OGG_HEADER_FIXED 27

#define REPRO_SERIALNO 0x47534231u
#define REPRO_PAYLOAD_MAGIC 0x5250u
#define REPRO_PAYLOAD_SIZE 120
#define REPRO_DEFAULT_PAGES 24u

/*
 * Every packet payload carries an application-level frame so recovery can be
 * verified independently of the Ogg page CRC:
 *
 *   uint16 magic   = REPRO_PAYLOAD_MAGIC
 *   uint16 pktno   = packet index (== page index, one packet per page)
 *   uint32 crc32   = CRC-32 (IEEE 802.3, reflected) over payload[8..end]
 *   uint8  data[]  = deterministic filler
 */

static inline uint16_t rd16(const unsigned char *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rd32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr16(unsigned char *p, uint16_t v) {
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
}

static inline void wr32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
  p[2] = (unsigned char)((v >> 16) & 0xff);
  p[3] = (unsigned char)((v >> 24) & 0xff);
}

static inline uint32_t repro_crc32(const unsigned char *p, size_t n) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++)
      crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1)));
  }
  return crc ^ 0xffffffffu;
}

/* Build one deterministic payload frame. Returns REPRO_PAYLOAD_SIZE. */
static inline size_t repro_make_payload(unsigned char *buf, uint16_t pktno) {
  wr16(buf + 0, REPRO_PAYLOAD_MAGIC);
  wr16(buf + 2, pktno);
  wr32(buf + 4, 0);
  for (int i = 8; i < REPRO_PAYLOAD_SIZE; i++)
    buf[i] = (unsigned char)((pktno * 31u + (unsigned)i * 7u) & 0xff);
  wr32(buf + 4, repro_crc32(buf + 8, REPRO_PAYLOAD_SIZE - 8));
  return REPRO_PAYLOAD_SIZE;
}

/* Validate a recovered payload frame. Returns the packet number, or -1. */
static inline int repro_check_payload(const unsigned char *buf, long n) {
  if (n != REPRO_PAYLOAD_SIZE || rd16(buf) != REPRO_PAYLOAD_MAGIC)
    return -1;
  uint32_t want = rd32(buf + 4);
  unsigned char tmp[REPRO_PAYLOAD_SIZE];
  for (int i = 0; i < REPRO_PAYLOAD_SIZE; i++) tmp[i] = buf[i];
  wr32(tmp + 4, 0);
  if (repro_crc32(tmp + 8, REPRO_PAYLOAD_SIZE - 8) != want)
    return -1;
  return (int)rd16(buf + 2);
}

#endif
