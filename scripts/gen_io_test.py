#!/usr/bin/env python3
"""Generate the deterministic 1 MiB test file for bench_io / bench_mix.

Usage: python3 scripts/gen_io_test.py <output_path>

Writes exactly 1 MiB of deterministic pseudo-random bytes (xorshift32).
The benchmarks checksum the data they read, so the file must be byte-identical
on every generation for checksums to be comparable across runs.
"""
import sys

SIZE = 1024 * 1024


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "build/io_test.bin"
    state = 0x12345678
    buf = bytearray(SIZE)
    for i in range(SIZE):
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        buf[i] = state & 0xFF
    with open(out_path, "wb") as f:
        f.write(buf)
    print(f"gen_io_test: wrote {SIZE} bytes to {out_path}")


if __name__ == "__main__":
    main()
