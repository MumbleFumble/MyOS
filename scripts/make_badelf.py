#!/usr/bin/env python3
"""Craft malformed ELFs for scripts/test_badelf.sh
(Phase 17 inc.4).

Usage: make_badelf.py <good-elf> <disk-image>

Reads a known-good user ELF, produces four mutated copies,
each breaking exactly one loader invariant, and appends
them to the MyFS image as badelf1.elf .. badelf4.elf
(record append in the pattern of test_shellscript.sh's
fixture injection):

  badelf1  phdr table extends past EOF (e_phoff moved to
           size-8, so phnum entries cannot fit)
  badelf2  first PT_LOAD's file range past EOF
           (p_offset=0x1000, p_filesz=p_memsz=size+0x10000,
           so the filesz<=memsz check passes and the range
           check is the one that must fire)
  badelf3  first PT_LOAD with p_filesz > p_memsz
  badelf4  e_entry outside every loaded segment
"""
import struct
import sys

EHDR_PHDR_OFF = 32        # e_phoff u64
EHDR_ENTRY_OFF = 24       # e_entry u64
PHDR_FILESZ_OFF = 32      # p_filesz u64 within Elf64_Phdr
PHDR_MEMSZ_OFF = 40       # p_memsz u64 within Elf64_Phdr
PHDR_OFFSET_OFF = 8       # p_offset u64 within Elf64_Phdr
PHDR_SIZE = 56


def first_load(data):
    phoff = struct.unpack_from('<Q', data, EHDR_PHDR_OFF)[0]
    phentsize = struct.unpack_from('<H', data, 54)[0]
    phnum = struct.unpack_from('<H', data, 56)[0]
    for i in range(phnum):
        off = phoff + i * phentsize
        if struct.unpack_from('<I', data, off)[0] == 1:  # PT_LOAD
            return off
    raise SystemExit("no PT_LOAD in good ELF")


def main():
    good_path, img_path = sys.argv[1], sys.argv[2]
    good = bytearray(open(good_path, 'rb').read())
    size = len(good)
    seg = first_load(good)

    b1 = bytearray(good)
    struct.pack_into('<Q', b1, EHDR_PHDR_OFF, size - 8)

    b2 = bytearray(good)
    struct.pack_into('<Q', b2, seg + PHDR_OFFSET_OFF, 0x1000)
    struct.pack_into('<Q', b2, seg + PHDR_FILESZ_OFF, size + 0x10000)
    struct.pack_into('<Q', b2, seg + PHDR_MEMSZ_OFF, size + 0x10000)

    b3 = bytearray(good)
    memsz = struct.unpack_from('<Q', good, seg + PHDR_MEMSZ_OFF)[0]
    struct.pack_into('<Q', b3, seg + PHDR_FILESZ_OFF, memsz + 0x100)

    b4 = bytearray(good)
    struct.pack_into('<Q', b4, EHDR_ENTRY_OFF, 0x400000)

    variants = {
        'badelf1.elf': b1,
        'badelf2.elf': b2,
        'badelf3.elf': b3,
        'badelf4.elf': b4,
    }

    img = bytearray(open(img_path, 'rb').read())
    total = len(img) // 512
    magic, count = struct.unpack_from('<II', img, 0)
    assert magic == 0x4D594653, hex(magic)
    limit = (total - 2048) * 512 if total > 3072 else total * 512
    off = 512
    for _ in range(count):
        fsize = struct.unpack_from('<Q', img, off + 64)[0]
        off += ((80 + fsize + 511) // 512) * 512
    for name, data in variants.items():
        hdr = bytearray(80)
        nb = name.encode()
        hdr[:len(nb)] = nb
        struct.pack_into('<Q', hdr, 64, len(data))
        rec = bytes(hdr) + bytes(data)
        rec += b'\0' * (((len(rec) + 511) // 512) * 512 - len(rec))
        assert off + len(rec) <= limit, "image full"
        img[off:off + len(rec)] = rec
        off += len(rec)
        count += 1
    struct.pack_into('<I', img, 4, count)
    open(img_path, 'wb').write(bytes(img))
    print("[badelf] injected badelf1.elf..badelf4.elf "
          f"(base size {size}, first PT_LOAD at {seg})")


if __name__ == '__main__':
    main()
