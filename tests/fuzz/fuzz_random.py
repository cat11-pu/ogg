#!/usr/bin/env python3
"""Extra randomized fuzzing round with fresh seeds.

Generates N arbitrary buffers (pure junk, mutated valid pages, random
truncations) and runs each through the sanitizer driver.  Any crash,
OOB, UBSan report or leak fails the round.
"""

import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from generate import one_base_page, multi_page_chain  # noqa: E402
from ogg_model import crc32ogg  # noqa: E402
import struct  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def make_buffer(rng):
    kind = rng.choice(["junk", "junk", "mutated_page", "splice",
                       "truncate_random", "page_soup"])
    if kind == "junk":
        return bytes(rng.randrange(256) for _ in range(rng.randint(0, 4096)))
    if kind == "mutated_page":
        page = bytearray(one_base_page(rng, nseg=rng.randint(0, 255)))
        for _ in range(rng.randint(1, 8)):
            idx = rng.randrange(len(page))
            page[idx] = rng.randrange(256)
        return bytes(page)
    if kind == "splice":
        good = one_base_page(rng)
        junk = bytes(rng.randrange(256) for _ in range(rng.randint(0, 64)))
        return junk + good + junk
    if kind == "truncate_random":
        page = one_base_page(rng)
        return page[:rng.randint(0, len(page))]
    if kind == "page_soup":
        out = bytearray()
        for _ in range(rng.randint(1, 5)):
            p = bytearray(one_base_page(rng, serial=rng.randint(0, 0xFFFFFFFF),
                                        pageno=rng.randint(0, 0xFFFFFFFF)))
            if rng.random() < 0.5:
                p[rng.randrange(27, len(p))] ^= 0xFF
                p[22:26] = struct.pack(
                    "<I", (~crc32ogg(bytes(p[:27 + p[26]] +
                                          p[27:27 + p[26]]))) & 0xFFFFFFFF)
            out.extend(p)
        return bytes(out)
    chain = multi_page_chain(rng, rng.randint(1, 6))
    return chain[:rng.randint(1, len(chain))]


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 1000
    seed = (int(sys.argv[2], 0) if len(sys.argv) > 2
            else 0x5EED5EED)
    rng = random.Random(seed)
    driver = os.path.join(HERE, "fuzz_parse")
    tmp = os.path.join(HERE, "out", "random_case.bin")
    failures = []
    for i in range(count):
        blob = make_buffer(rng)
        with open(tmp, "wb") as fh:
            fh.write(blob)
        proc = subprocess.run([driver, tmp], capture_output=True, text=True)
        if proc.returncode != 0:
            failures.append({"index": i, "rc": proc.returncode,
                             "stderr": proc.stderr[-1500:]})
            break
        if (i + 1) % 200 == 0:
            print(f"  {i + 1}/{count} clean", flush=True)
    if failures:
        for f in failures:
            print("FAILURE", f)
        sys.exit(1)
    print(f"RANDOM FUZZ CLEAN: {count} buffers, seed={seed}")


if __name__ == "__main__":
    main()
