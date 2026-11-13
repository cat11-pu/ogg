# Ogg framing parse/checksum benchmark

`bench.c` measures the hot decode path against a real libogg build and
dumps every parsed field for byte-exact regression checks.

## Build two trees

```sh
cmake -S . -B build-baseline -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-baseline -j
cmake -S . -B build-opt      -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-opt -j

gcc -O3 -DNDEBUG -Iinclude -Ibuild-baseline/include bench/bench.c \
    build-baseline/libogg.a -o bench/bench-base
gcc -O3 -DNDEBUG -Iinclude      -Ibuild-opt/include bench/bench.c \
    build-opt/libogg.a      -o bench/bench-opt
```

## Field-by-field equivalence

Both binaries use the same deterministic PRNG, so the encoded stream is
identical. Diff the full parse dump (page header fields, segment table,
per-packet flags/granule/bytes and an FNV payload fingerprint):

```sh
./bench/bench-base check > base.txt
./bench/bench-opt  check > opt.txt
diff -u base.txt opt.txt   # no output == identical results
```

## Timing across page lengths

`time` reports, for seven representative body lengths (0 .. 65025, the
Ogg maximum), the per-page nanoseconds and throughput for:

- `crc`  - `ogg_page_checksum_set` over header+body only
- `sync` - `ogg_sync_pageout` (capture, segment sum, CRC verify)
- `full` - sync plus `ogg_stream_pagein`/`packetout`

Each figure is the best (minimum) of 15 trials after a warm-up; pinning
to one core reduces scheduler noise:

```sh
taskset -c 7 ./bench/bench-base time > time-base.txt
taskset -c 7 ./bench/bench-opt  time > time-opt.txt
python3 bench/compare.py time-base.txt time-opt.txt
```
