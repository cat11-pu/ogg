# Hardening summary: malformed Ogg pages

## Defects reproduced on the unmodified tree (ASan + UBSan, gcc 15)

| Defect class | Trigger sample | Baseline result |
|--------------|----------------|-----------------|
| Orphan continuation, segments exceed body | `continue_orphan_overrun.bin` | `negative-size-param` in `memcpy` at `src/framing.c` (old line 862) |
| Continuation skip underflow | `continue_skip_underflow.bin` | `negative-size-param (size=-1021)` in `memcpy` |
| Corrupt segment table / packet over-read | `poc_stateful.c` lacing case | `heap-buffer-overflow` READ past 16 KB `body_data` |
| Any malformed random stream | `fuzz_random` seed 1 | `negative-size-param`, process abort (exit 1) |
| Wrong page number + inconsistent lacing | `seq_backwards_overrun.bin` | `body_fill`/lacing bookkeeping corruption (no reliable length) |

Evidence: `reports/baseline_crash.txt`, `reports/baseline_fuzz_crash.txt`.

## Behavior after the fix

Every malformed page is rejected before any state change. `ogg_stream_pagein()`
still returns `-1`; the reason and byte offset are available from
`ogg_stream_errcode()` / `ogg_stream_erroff()`:

```
continue_orphan_overrun.bin  raw page[0] rc=-1 errcode=-5 erroff=27
continue_skip_underflow.bin  raw page[0] rc=-1 errcode=-5 erroff=27
corrupt_seg_overrun.bin      raw page[0] rc=-1 errcode=-5 erroff=27
corrupt_bad_version.bin      raw page[0] rc=-1 errcode=-2 erroff=4
garbage_prefix.bin (raw)     captured at off=12, accepted page rc=0
valid_stream.ogg             both pages rc=0, 3 packets decoded
```

## Zero-crash statistics (fixed build, `halt_on_error=1`)

| Campaign | Volume | Sanitizer failures |
|----------|--------|--------------------|
| Fixed corpus, sync + raw pipelines | 28 runs over 14 files | 0 |
| Stateful PoCs (`poc_stateful`) | 2 attack scenarios | 0 |
| Random fuzz, 8 seeds | 24,000 iterations | 0 |
| — pages synced | 6,901 | 0 |
| — malformed pages rejected with code | 46,953 | 0 |
| — well-formed pages accepted | 12,310 | 0 |
| — packet bytes drained/checked | 1,123,492 packet reads | 0 |
| — sync holes recovered | 21,445 | 0 |
| `run_all.sh` aggregate | 38 pipeline runs + 16,000 fuzz iterations | **0 — ZERO CRASHES** |

## Valid-stream equivalence

`roundtrip` built against the original tree and the fixed tree produce
byte-identical packet metadata and content hashes for `valid_stream.ogg`
(see `reports/roundtrip_baseline.txt` vs `reports/roundtrip_fixed.txt`).
The upstream self-tests (`test_framing`, `test_bitwise`) pass under both
autotools `make check` and CMake with zero compiler warnings.
