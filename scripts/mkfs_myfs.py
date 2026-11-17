#!/usr/bin/env python3
"""
mkfs_myfs.py — build a MyFS disk image.

Usage:
    python3 scripts/mkfs_myfs.py output.img file1 file2 ...

Each positional argument after the output path is a file to embed.
The file's basename becomes its name in the filesystem.
"""

import sys
import os
import struct

MYFS_MAGIC    = 0x4D594653
SECTOR_SIZE   = 512
MYFS_NAME_MAX = 64

def pad_to_sector(data: bytes) -> bytes:
    rem = len(data) % SECTOR_SIZE
    if rem:
        data += b'\x00' * (SECTOR_SIZE - rem)
    return data

def build_image(out_path: str, files: list[str]) -> None:
    # Superblock: magic(4) + count(4) + padding to 512
    super_block = struct.pack('<II', MYFS_MAGIC, len(files))
    super_block += b'\x00' * (SECTOR_SIZE - len(super_block))

    records = b''
    for path in files:
        name = os.path.basename(path).encode('ascii')
        if len(name) >= MYFS_NAME_MAX:
            name = name[:MYFS_NAME_MAX - 1]
        name = name.ljust(MYFS_NAME_MAX, b'\x00')

        with open(path, 'rb') as f:
            data = f.read()

        # File header: name(64) + size(8) + pad(8) = 80 bytes
        hdr = name + struct.pack('<Q', len(data)) + b'\x00' * 8
        assert len(hdr) == 80

        record = hdr + data
        record = pad_to_sector(record)
        records += record
        print(f"  added {os.path.basename(path)} ({len(data)} bytes)")

    image = super_block + records
    # Pad the image with zero sectors to a fixed total size. The tail
    # is free space: MyFS appends (Phase 6 inc.3) grow into it, and the
    # last 2048 sectors are the raw-sector test scratch area. Without
    # the padding the image is exactly content-sized and its tail
    # sectors are live file data.
    TOTAL_SECTORS = 32768
    content_sectors = len(image) // SECTOR_SIZE
    assert content_sectors <= TOTAL_SECTORS - 2048, \
        f"MyFS content ({content_sectors} sectors) leaves no free tail"
    if content_sectors < TOTAL_SECTORS:
        image += b'\x00' * ((TOTAL_SECTORS - content_sectors) * SECTOR_SIZE)
    with open(out_path, 'wb') as f:
        f.write(image)
    print(f"MyFS image: {out_path} ({len(image)} bytes, {len(image)//SECTOR_SIZE} sectors)")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} output.img [file ...]")
        sys.exit(1)
    build_image(sys.argv[1], sys.argv[2:])
