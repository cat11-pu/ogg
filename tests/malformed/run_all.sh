#!/bin/sh
# Full malformed-page regression under ASan/UBSan. Exits non-zero on any
# sanitizer report, otherwise prints a zero-crash summary.
set -e
cd "$(dirname "$0")"

export ASAN_OPTIONS="halt_on_error=1:detect_leaks=0:strict_string_checks=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"

python3 gen_samples.py >/dev/null
make clean >/dev/null
make >/dev/null

crash=0
runs=0
scan() {
  runs=$((runs + 1))
  if echo "$1" | grep -qE "ERROR: AddressSanitizer|runtime error:|SUMMARY: (Address|Undefined)"; then
    echo "SANITIZER FAILURE: $2"
    echo "$1"
    crash=$((crash + 1))
  fi
}

for f in corpus/*; do
  scan "$(./build/fuzz_pagein "$f" 2>&1)" "sync $(basename "$f")"
  scan "$(./build/fuzz_pagein --raw "$f" 2>&1)" "raw $(basename "$f")"
done

scan "$(./build/poc_stateful 2>&1)" "stateful PoC"
scan "$(./build/roundtrip corpus/valid_stream.ogg 2>&1)" "valid roundtrip"

fuzz_total=0
for seed in 1 2 3 7 42 99 123456 20261118; do
  out="$(./build/fuzz_random 2000 "$seed" 2>&1)"
  scan "$out" "random fuzz seed=$seed"
done

echo
echo "pipeline_runs=$runs sanitizer_failures=$crash"
if [ "$crash" -ne 0 ]; then
  echo "RESULT: FAIL"
  exit 1
fi
echo "RESULT: ZERO CRASHES"
