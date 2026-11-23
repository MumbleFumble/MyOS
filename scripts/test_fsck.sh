#!/usr/bin/env bash
# Host-side test for scripts/fsck_myfs.py (Phase 13 inc.1).
# No QEMU needed: fsck is a host tool. Asserts:
#   1. a fresh mkfs image reports CLEAN and fsck leaves the
#      image byte-identical (read-only);
#   2. a hand-built append-only image (tombstoned old record +
#      live rewrite of the same name) also reports CLEAN;
#   3. four deliberate corruptions are each detected with a
#      non-zero exit and a violation naming the invariant:
#      bad super magic, inflated record size (overrun), a
#      cleared tombstone creating two live records with one
#      name, and an undefined tombstone flag byte.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d /tmp/myos_fsck.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

fail() { echo "[test] FAIL: $1"; exit 1; }

FRESH="$TMP/fresh.img"
python3 "$ROOT/scripts/mkfs_myfs.py" "$FRESH" \
    "$ROOT/disk_files/README.txt" "$ROOT/disk_files/test.txt" > /dev/null \
    || fail "mkfs_myfs.py could not build the fresh image"

H1="$(sha256sum "$FRESH" | cut -d' ' -f1)"
OUT="$(python3 "$ROOT/scripts/fsck_myfs.py" "$FRESH")" || fail "fsck rejected the fresh image: $OUT"
echo "$OUT" | grep -q "CLEAN" || fail "fresh image not CLEAN: $OUT"
echo "[test] fresh image: $OUT"
H2="$(sha256sum "$FRESH" | cut -d' ' -f1)"
[ "$H1" = "$H2" ] || fail "fsck modified the image (hash changed)"

# Image B: append-only history built by hand — a.txt written,
# tombstoned, rewritten live; b.txt live. Must be CLEAN.
python3 - "$TMP/history.img" << 'PYEOF' || fail "could not build the history image"
import struct, sys
def rec(name, data, deleted=False):
    hdr = name.encode().ljust(64, b"\x00") + struct.pack("<Q", len(data)) \
        + bytes([1 if deleted else 0]) + b"\x00" * 7
    r = hdr + data
    return r + b"\x00" * ((512 - len(r) % 512) % 512)
img = struct.pack("<II", 0x4D594653, 3) + b"\x00" * 504
img += rec("a.txt", b"old", deleted=True)
img += rec("a.txt", b"new!")
img += rec("b.txt", b"x")
img += b"\x00" * (32768 * 512 - len(img))
open(sys.argv[1], "wb").write(img)
PYEOF
OUT="$(python3 "$ROOT/scripts/fsck_myfs.py" "$TMP/history.img")" || fail "fsck rejected the history image: $OUT"
echo "$OUT" | grep -q "3 records walked, 2 live files, 1 tombstones" \
    || fail "history image summary wrong: $OUT"
echo "[test] history image: $OUT"

expect_bad() { # image, keyword, label
    local out rc
    out="$(python3 "$ROOT/scripts/fsck_myfs.py" "$1" 2>&1)"; rc=$?
    [ $rc -ne 0 ] || fail "$3: fsck exited 0 on a corrupted image"
    echo "$out" | grep -qi "$2" || fail "$3: violation does not name '$2': $out"
    echo "[test] corruption caught ($3): $(echo "$out" | head -1)"
}

# 1. Bad super magic.
cp "$FRESH" "$TMP/badmagic.img"
printf '\x00' | dd of="$TMP/badmagic.img" bs=1 seek=0 count=1 conv=notrunc 2>/dev/null
expect_bad "$TMP/badmagic.img" "magic" "bad super magic"

# 2. Inflated record size: record 0's size field (offset 512+64)
#    set to 16 MiB so the record overruns the append limit.
cp "$FRESH" "$TMP/bigsize.img"
python3 - "$TMP/bigsize.img" << 'PYEOF'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    f.seek(512 + 64)
    f.write(struct.pack("<Q", 0x01000000))
PYEOF
expect_bad "$TMP/bigsize.img" "overrun" "inflated record size"

# 3. Cleared tombstone: image B's old a.txt record (sector 1,
#    flag byte at 512+72) becomes live again — two live a.txt.
cp "$TMP/history.img" "$TMP/duplive.img"
printf '\x00' | dd of="$TMP/duplive.img" bs=1 seek=$((512 + 72)) count=1 conv=notrunc 2>/dev/null
expect_bad "$TMP/duplive.img" "duplicate" "cleared tombstone / duplicate live record"

# 4. Undefined tombstone flag byte (0x07) on record 0.
cp "$FRESH" "$TMP/badflag.img"
printf '\x07' | dd of="$TMP/badflag.img" bs=1 seek=$((512 + 72)) count=1 conv=notrunc 2>/dev/null
expect_bad "$TMP/badflag.img" "flag" "undefined tombstone flag byte"

echo "[test] PASS: fsck CLEAN on fresh + history images, read-only, 4 corruptions detected"
exit 0
