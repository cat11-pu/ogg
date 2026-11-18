# Malformed-page hardening tests

This directory proves that the Ogg framing decoder rejects every malformed
page with an **error code and a byte offset** instead of crashing, reading
or writing out of bounds, or silently corrupting stream state — while the
decoded output for every valid stream stays byte-for-byte identical.

## The three defect classes

All three were reachable through `ogg_stream_pagein()` (the sync layer's CRC
and length gates stop most of them on a normal file, but any caller that
hands a page straight to the stream decoder — muxers, seeking tools, fuzzed
or truncated streams with a valid-looking CRC — hit the unsafe code).

1. **Corrupt page** — segment table whose summed lengths do not match
   `body_len`, truncated header/segment table, bad capture or version.
2. **Cross-page continuation packet** — `continued` flag set with no partial
   packet pending, whose leading segments exceed the available body; the
   skip loop drove `bodysize` negative and reached `memcpy(..., -n)`.
3. **Wrong sequence number** — duplicate / backwards / hole page numbers; the
   partial-packet unroll could drive `body_fill` negative, and inconsistent
   lacing then let `ogg_stream_packetout()` report packets longer than the
   bytes actually stored (heap over-read).

## The fix

`ogg_stream_pagein()` now **fully validates a page before mutating any
stream state**:

- non-NULL header, at least 27 bytes, non-negative body length, `OggS`
  capture;
- the declared segment table fits inside `header_len`;
- stream structure version is `0`;
- serial number matches the stream;
- the sum of the 255-capped segment values (with overflow checking) equals
  `body_len` exactly — this single invariant removes every negative
  `memcpy`, over-read and bookkeeping underflow;
- a continuation-skip that would exceed the body is rejected up front.

Rejections still return the historical `-1` (API/ABI compatible) and now also
record a reason and offset on the stream state:

| code | macro | meaning | typical offset |
|------|-------|---------|----------------|
| `-1` | `OGG_ERR_BAD_HEADER` | short/non-`OggS` header, negative body | 0 |
| `-2` | `OGG_ERR_BAD_VERSION` | unsupported version byte | 4 |
| `-3` | `OGG_ERR_SERIAL_MISMATCH` | serial does not match | 14 |
| `-4` | `OGG_ERR_TRUNCATED` | segment table runs past header | 26 |
| `-5` | `OGG_ERR_BODY_MISMATCH` | segment sum != body length | 27 / 27+i |
| `-6` | `OGG_ERR_CONTINUATION` | orphan continuation overrun | 27 |
| `-7` | `OGG_ERR_ALLOC` | storage allocation failed | -1 |

Read them back with `ogg_stream_errcode(os)` and `ogg_stream_erroff(os)`.
`_os_body_expand()` / `_os_lacing_expand()` also reject negative `needed`.
The gap-unroll loop clamps instead of ever underflowing `body_fill`.

## Layout

- `gen_samples.py` — regenerates `corpus/` (a valid reference stream plus
  one file per defect class and truncation/noise cases).
- `fuzz_pagein.c` — drives a file through the public sync+stream pipeline,
  or (`--raw`) splits it into pages and bypasses the CRC gate.
- `poc_stateful.c` — hand-built pages for the lacing over-read and
  sequence-underflow paths.
- `roundtrip.c` — prints packet metadata + content hash for valid streams;
  output must be identical before and after the fix.
- `fuzz_random.c` — randomized malformed-stream fuzzer (segments, bodies,
  flags, serials, page numbers, byte corruption), sync and raw paths.
- `reports/` — captured before/after evidence and run statistics.

## Reproduce

```sh
cd tests/malformed
python3 gen_samples.py
make                       # ASan + UBSan build of the current (fixed) tree
for f in corpus/*; do ./build/fuzz_pagein "$f"; \
  ./build/fuzz_pagein --raw "$f"; done
./build/poc_stateful
./build/roundtrip corpus/valid_stream.ogg
./build/fuzz_random 10000 20261118
```

The whole suite must finish with no AddressSanitizer/UBSan report.
