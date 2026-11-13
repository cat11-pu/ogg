#!/usr/bin/env bash
#
# End-to-end reproduction of "one bad page kills the rest of the stream"
# and the fixed resync behavior. Produces:
#   clean.ogg          reference stream
#   corrupt_body.ogg   page 8 damaged in the body (OggS header intact)
#   corrupt_header.ogg page 8 with a destroyed capture header
#
set -euo pipefail

cd "$(dirname "$0")"

NPAGES="${NPAGES:-24}"
BAD="${BAD:-8}"
BAD_A="${BAD_A:-5}"
BAD_B="${BAD_B:-17}"

make -s

echo "== 1. generate clean stream ($NPAGES pages) =="
./gen_stream clean.ogg "$NPAGES"

echo
echo "== 2. corrupt page $BAD (two corruption models) =="
./corrupt_stream clean.ogg corrupt_body.ogg   "$BAD" body
./corrupt_stream clean.ogg corrupt_header.ogg "$BAD" header

echo
echo "== 3a. clean stream through the naive decoder (control) =="
./decode_naive clean.ogg || true

echo
echo "== 3b. corrupted stream through the naive decoder (shows the bug) =="
set +e
./decode_naive corrupt_body.ogg
naive_rc=$?
set -e
echo "naive decoder exit code: $naive_rc (non-zero = it gave up permanently)"

for f in corrupt_body.ogg corrupt_header.ogg; do
  echo
  echo "== 4. fixed decoder on $f =="
  ./decode_robust "$f"
done

echo
echo "== 5. machine-readable stats (body corruption) =="
./decode_robust corrupt_body.ogg --json

echo
echo "== 6. streaming feed in 33-byte chunks (pages span chunk boundaries) =="
./decode_robust corrupt_body.ogg --chunksize 33

echo
echo "== 7. two independent bad pages ($BAD_A and $BAD_B): counting check =="
./corrupt_stream clean.ogg      tmp_step1.ogg "$BAD_A" body
./corrupt_stream tmp_step1.ogg  corrupt_two.ogg "$BAD_B" body
rm -f tmp_step1.ogg
./decode_robust corrupt_two.ogg
