#!/usr/bin/env python3
"""fsck_myfs.py — read-only invariant checker for MyFS disk images.

Usage: python3 scripts/fsck_myfs.py <image>

Walks the superblock and the record log exactly as the kernel
mount walk does (src/kernel/fs/diskfs.c diskfs_mount) and reports
every invariant violation it finds, one line per violation.
Exit 0 with a CLEAN summary when the image is well-formed, 1 when
violations are found, 2 on usage/IO errors. The image is opened
read-only and never modified.

The format (src/kernel/fs/myfs.h): sector 0 is the superblock
(magic 0x4D594653, file_count, padded to 512); from sector 1,
records follow, each an 80-byte header (name[64], uint64 size,
_pad[8] with _pad[0] as the flags byte) + data, the whole record
padded to a 512-byte multiple. Updates are append-only: rewriting
a file tombstones its old record in place (MYFS_FLAG_DELETED
0x01 in _pad[0]) and appends the new content; the latest live
record for a name wins.

What file_count means (diskfs.c): the mount comment states it
plainly — "Walk ALL records (the superblock count includes
tombstones and superseded records)" — and diskfs_write_super
rewrites it from disk_record_count, which diskfs_append_record
increments on EVERY append. So file_count is the total number of
records in the log, not the number of live files, and the walk
consumes exactly that many records starting at sector 1.
"""
import struct
import sys

MAGIC = 0x4D594653          # MYFS_MAGIC (myfs.h)
SECTOR = 512
NAME_MAX = 64               # MYFS_NAME_MAX (myfs.h)
HDR_LEN = 80                # sizeof(myfs_file_hdr_t)
FLAG_DELETED = 0x01         # MYFS_FLAG_DELETED (myfs.h)
MAX_FILES = 64              # DISKFS_MAX_FILES (diskfs.c)
SCRATCH_SECTORS = 2048      # reserved tail (diskfs.c mount)
# The mount walk computes record sectors in uint32 arithmetic:
# (80 + (uint32_t)size + 511) / 512. A size above this wraps it.
SIZE_WRAP_LIMIT = 0xFFFFFFFF - 591


def main(argv):
    if len(argv) != 2:
        print(f"usage: {argv[0]} <image>", file=sys.stderr)
        return 2
    try:
        with open(argv[1], "rb") as f:
            data = f.read()
    except OSError as e:
        print(f"fsck: cannot read {argv[1]}: {e}", file=sys.stderr)
        return 2

    violations = []

    def vio(invariant, offset, detail):
        violations.append(f"VIOLATION {invariant} at offset {offset}: {detail}")

    if len(data) % SECTOR != 0:
        vio("image-size", len(data),
            f"image size {len(data)} is not a multiple of the 512-byte sector")
        data = data[: len(data) - (len(data) % SECTOR)]
    total_sectors = len(data) // SECTOR
    if total_sectors < 1:
        vio("image-size", 0, "image has no superblock sector")
        for v in violations:
            print(v)
        print(f"fsck: {len(violations)} violation(s) found")
        return 1

    magic, file_count = struct.unpack_from("<II", data, 0)
    if magic != MAGIC:
        vio("super-magic", 0,
            f"superblock magic 0x{magic:08X} != MYFS_MAGIC 0x{MAGIC:08X} "
            "(diskfs_mount refuses to mount)")
        for v in violations:
            print(v)
        print(f"fsck: {len(violations)} violation(s) found")
        return 1

    # The mount walk stops at disk_append_limit = total - 2048
    # (diskfs.c); records must live below it, clear of the
    # raw-sector scratch area in the last 2048 sectors.
    limit_sectors = total_sectors - SCRATCH_SECTORS \
        if total_sectors > SCRATCH_SECTORS + 1024 else total_sectors
    limit_bytes = limit_sectors * SECTOR

    cur = SECTOR  # records start at sector 1 (myfs.h)
    live = {}     # name -> record index of its latest live record
    names = set()  # every distinct name the walk folds into the table
    tombstones = 0
    walked = 0

    for i in range(file_count):
        off = cur
        if off >= limit_bytes:
            vio("file-count", off,
                f"mount walk reaches the append limit at record {i} of "
                f"{file_count}: superblock file_count overstates the log")
            break
        if off + HDR_LEN > len(data):
            vio("record-overrun", off,
                f"record {i} header runs past the end of the image")
            break
        hdr = data[off:off + HDR_LEN]
        name_raw = hdr[:NAME_MAX]
        size = struct.unpack_from("<Q", hdr, 64)[0]
        flag = hdr[72]  # _pad[0]: 64 name + 8 size (diskfs_tombstone)

        nul = name_raw.find(b"\x00")
        if nul < 0:
            vio("name-termination", off,
                f"record {i} name has no NUL within {NAME_MAX} bytes "
                "(the mount walk truncates such names to 63 chars)")
            name = name_raw.decode("latin-1")
        elif nul == 0:
            vio("name-empty", off,
                f"record {i} name is empty (no producer writes one: "
                "diskfs_create rejects empty names)")
            name = ""
        else:
            name = name_raw[:nul].decode("latin-1")

        if flag not in (0x00, FLAG_DELETED):
            vio("tombstone-flag", off,
                f"record {i} ({name!r}) flag byte 0x{flag:02X} is not "
                "0x00 or MYFS_FLAG_DELETED 0x01 (myfs.h defines no "
                "other flag)")

        if size > SIZE_WRAP_LIMIT:
            vio("size-sanity", off,
                f"record {i} ({name!r}) size {size} overflows the "
                "mount walk's uint32 record-sector arithmetic")
            break
        rec_sects = (HDR_LEN + size + SECTOR - 1) // SECTOR
        rec_end = off + rec_sects * SECTOR
        if rec_end > limit_bytes:
            vio("record-overrun", off,
                f"record {i} ({name!r}) size {size} ends at byte "
                f"{rec_end}, past the append limit {limit_bytes} "
                "(records tile contiguously below total-2048 sectors)")
            break

        names.add(name)
        if flag & FLAG_DELETED:
            tombstones += 1
            live.pop(name, None)
        else:
            if name in live:
                vio("duplicate-live", off,
                    f"record {i} ({name!r}) is live but record "
                    f"{live[name]} with the same name is also live "
                    "(a rewrite tombstones the old record first; "
                    "latest-wins leaves exactly one live record "
                    "per name)")
            live[name] = i
        walked += 1
        cur = rec_end

    if len(names) > MAX_FILES:
        vio("too-many-files", SECTOR,
            f"{len(names)} distinct names exceed DISKFS_MAX_FILES "
            f"{MAX_FILES} (the mount walk silently drops the rest)")

    if violations:
        for v in violations:
            print(v)
        print(f"fsck: {len(violations)} violation(s) found")
        return 1
    print(f"fsck: CLEAN — {walked} records walked, {len(live)} live "
          f"files, {tombstones} tombstones")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
