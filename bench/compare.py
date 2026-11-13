#!/usr/bin/env python3
"""Compare two `bench <name> time` outputs and print speedup tables."""
import sys


def load(path):
    rows = {}
    with open(path) as fh:
        for line in fh:
            a = line.split()
            if len(a) == 9 and a[0].isdigit():
                rows[int(a[0])] = {
                    "page": int(a[1]), "n": int(a[2]),
                    "crc": float(a[3]), "sync": float(a[4]),
                    "full": float(a[5]),
                    "cg": float(a[6]), "sg": float(a[7]), "fg": float(a[8]),
                }
    return rows


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: compare.py baseline.txt optimized.txt")
    base, opt = load(sys.argv[1]), load(sys.argv[2])
    # for per-page nanoseconds, lower is better
    gain_ns = lambda b, o: (b / o - 1.0) * 100.0
    # for throughput, higher is better
    gain_gb = lambda b, o: (o / b - 1.0) * 100.0
    print(f"{'body':>6} {'page':>6} | "
          f"{'CRC ns base->opt':>17} {'gain':>7} | "
          f"{'sync base->opt':>17} {'gain':>7} | "
          f"{'full base->opt':>17} {'gain':>7}")
    for k in base:
        b, o = base[k], opt[k]
        print(f"{k:>6} {b['page']:>6} | "
              f"{b['crc']:>8.1f}/{o['crc']:<8.1f} {gain_ns(b['crc'],o['crc']):>6.1f}% | "
              f"{b['sync']:>8.1f}/{o['sync']:<8.1f} {gain_ns(b['sync'],o['sync']):>6.1f}% | "
              f"{b['full']:>8.1f}/{o['full']:<8.1f} {gain_ns(b['full'],o['full']):>6.1f}%")
    print("\nthroughput GB/s:")
    for k in base:
        b, o = base[k], opt[k]
        print(f"body={k:>5}: CRC {b['cg']:.2f}->{o['cg']:.2f} ({gain_gb(b['cg'],o['cg']):+.0f}%)  "
              f"sync {b['sg']:.2f}->{o['sg']:.2f} ({gain_gb(b['sg'],o['sg']):+.0f}%)  "
              f"full {b['fg']:.2f}->{o['fg']:.2f} ({gain_gb(b['fg'],o['fg']):+.0f}%)")


if __name__ == "__main__":
    main()
