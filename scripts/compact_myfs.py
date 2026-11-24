#!/usr/bin/env python3
"""compact_myfs.py — offline compaction for MyFS disk images.

Usage: python3 scripts/compact_myfs.py <image>

Rewrites a MyFS image in place keeping only the live records:
the latest live record per name, in its original log order,
copied byte-for-byte. Tombstones and superseded records are
dropped, the superblock record count becomes the live count,
and the log region past the compacted records is zeroed, so
the guest mounts the result and appends continue immediately
after the last live record. The image size and the reserved
scratch tail (last 2048 sectors) are preserved verbatim.

Safety: the image is first validated with exactly the
invariant checks scripts/fsck_myfs.py performs (same parsing
rules, same limits). If any check fails the tool prints the
violations, exits 1, and leaves the image unmodified. The
rewrite itself goes to a temporary file in the same directory
and is moved over the original with os.replace, so a failure
mid-write cannot leave a half-compacted image.

Exit 0 on success, 1 when the image fails validation, 2 on
usage/IO errors.
"""
import os
import struct
import sys

MAGIC = 0x4D594653          # MYFS_MAGIC (myfs.h)
SECTOR = 512
NAME_MAX = 64               # MYFS_NAME_MAX (myfs.h)
HDR_LEN = 80                # sizeof(myfs_file_hdr_t)
FLAG_DELETED = 0x01         # MYFS_FLAG_DELETED (myfs.h)
MAX_FILES = 64              # DISKFS_MAX_FILES (diskfs.c)
SCRATCH_SECTORS = 2048      # reserved tail (diskfs.c mount)
SIZE_WRAP_LIMIT = 0xFFFFFFFF - 591


def walk(data):
    """Parse and validate exactly like fsck_myfs.py.

    Returns (violations, records, live) where records is a list
    of (index, name, flagged, offset, rec_sects) for every
    record walked, and live maps name -> winning record index
    for names whose latest record is live.
    """
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
        return violations, [], {}

    magic, file_count = struct.unpack_from("<II", data, 0)
    if magic != MAGIC:
        vio("super-magic", 0,
            f"superblock magic 0x{magic:08X} != MYFS_MAGIC 0x{MAGIC:08X} "
            "(diskfs_mount refuses to mount)")
        return violations, [], {}

    limit_sectors = total_sectors - SCRATCH_SECTORS \
        if total_sectors > SCRATCH_SECTORS + 1024 else total_sectors
    limit_bytes = limit_sectors * SECTOR

    cur = SECTOR
    live = {}
    names = set()
    records = []

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
        flag = hdr[72]

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
        records.append((i, name, bool(flag & FLAG_DELETED), off, rec_sects))
        if flag & FLAG_DELETED:
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
        cur = rec_end

    if len(names) > MAX_FILES:
        vio("too-many-files", SECTOR,
            f"{len(names)} distinct names exceed DISKFS_MAX_FILES "
            f"{MAX_FILES} (the mount walk silently drops the rest)")

    return violations, records, live


def main(argv):
    if len(argv) != 2:
        print(f"usage: {argv[0]} <image>", file=sys.stderr)
        return 2
    path = argv[1]
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        print(f"compact: cannot read {path}: {e}", file=sys.stderr)
        return 2

    violations, records, live = walk(data)
    if violations:
        for v in violations:
            print(v)
        print(f"compact: REFUSED — {len(violations)} violation(s); "
              "image left unmodified")
        return 1

    total_sectors = len(data) // SECTOR
    limit_sectors = total_sectors - SCRATCH_SECTORS \
        if total_sectors > SCRATCH_SECTORS + 1024 else total_sectors
    limit_bytes = limit_sectors * SECTOR

    by_index = {r[0]: r for r in records}
    winners = [by_index[i] for i in sorted(live.values())]
    old_used = (records[-1][3] // SECTOR + records[-1][4] - 1) if records else 0
    new_used = sum(r[4] for r in winners)

    out = bytearray(data)
    out[SECTOR:limit_bytes] = b"\x00" * (limit_bytes - SECTOR)
    cur = SECTOR
    for _, _, _, off, rec_sects in winners:
        n = rec_sects * SECTOR
        out[cur:cur + n] = data[off:off + n]
        cur += n
    struct.pack_into("<I", out, 4, len(winners))

    tmp = path + ".compact-tmp"
    try:
        with open(tmp, "wb") as f:
            f.write(out)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except OSError as e:
        print(f"compact: cannot write {path}: {e}", file=sys.stderr)
        try:
            os.unlink(tmp)
        except OSError:
            pass
        return 2

    print(f"compact: {len(records)} records -> {len(winners)} live records, "
          f"reclaimed {old_used - new_used} sectors")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
