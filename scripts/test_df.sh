#!/usr/bin/env bash
# Headless test for Phase 13 inc.5: shell `df` (MyFS capacity
# reporting).
#
# One boot on a small custom image (mkfs output truncated to
# 3073 sectors, so the append limit is 1025 and capacity is
# 1024 sectors). The guest builds a known state:
#   write a.txt aaaa   -> a live
#   write b.txt bbbb   -> b live
#   write a.txt a2a2   -> a's first record is non-tail by now:
#                         tombstoned; a's new record live
#   rm b.txt           -> b non-tail: tombstoned
#   df                 -> the figures under test
# Final state: 5 records walked, 3 live (README.txt, test.txt,
# a.txt), 2 tombstones — both non-tail, so nothing about this
# state is reachable by the tail rollbacks alone.
#
# After the boot, every printed df figure is compared against
# an independent host-side walk of the same image (the fsck
# oracle's parsing, extended to sector extents), and the
# record counts are also compared against fsck_myfs.py's own
# printed summary for the same image.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
TMP="$(mktemp -d /tmp/myos_df.XXXXXX)"

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }
command -v qemu-system-x86_64 >/dev/null || { echo "[test] FAIL: qemu-system-x86_64 missing"; exit 1; }

for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
sleep 1

QPID=""
cleanup() { [ -n "$QPID" ] && kill -9 "$QPID" 2>/dev/null; rm -rf "$TMP"; }
fail() { echo "[test] FAIL: $1"; cleanup; exit 1; }

boot() { # image serial sock
  rm -f "$2" "$3"
  qemu-system-x86_64 -m 256 -display none -vga none \
    -serial "file:$2" -monitor "unix:$3,server,nowait" \
    -boot d -cdrom "$ISO" -drive "file=$1,format=raw,if=ide,index=0" \
    -netdev "socket,id=n0,listen=127.0.0.1:10000" \
    -device "e1000,netdev=n0,mac=52:54:00:12:34:56" &
  QPID=$!
  local i
  for i in $(seq 1 40); do [ -S "$3" ] && break; sleep 0.5; done
  sleep 20
}

type_word() {
  local str="$1" i c
  for (( i=0; i<${#str}; i++ )); do
    c="${str:$i:1}"
    case "$c" in
      ' ') echo "sendkey spc" ;;
      '.') echo "sendkey dot" ;;
      '_') echo "sendkey shift-minus" ;;
      '-') echo "sendkey minus" ;;
      *)   echo "sendkey $c" ;;
    esac
    sleep 0.06
  done
  echo "sendkey ret"
  sleep 0.6
}
send_cmds() { # sock, file, inter-line delay (default 0.7)
  { while IFS= read -r line; do type_word "$line"; sleep "${3:-0.7}"; done < "$2"; } \
    | socat - UNIX-CONNECT:"$1" >/dev/null 2>&1 || true
}
count() { grep -a -c "$1" "$2" 2>/dev/null || true; }
wait_count() { # serial, pattern, wanted, budget_secs
  local i
  for i in $(seq 1 "$4"); do
    [ "$(count "$2" "$1")" -ge "$3" ] && return 0
    sleep 1
  done
  return 1
}
shutdown() { kill -9 "$QPID" 2>/dev/null; QPID=""; sleep 1; }

SMALL_BYTES=$((3073 * 512))
python3 "$ROOT/scripts/mkfs_myfs.py" "$TMP/df.img" \
    "$ROOT/disk_files/README.txt" "$ROOT/disk_files/test.txt" > /dev/null \
  || fail "mkfs failed"
truncate -s "$SMALL_BYTES" "$TMP/df.img"

printf '%s\n' \
  "write a.txt aaaa" \
  "write b.txt bbbb" \
  "write a.txt a2a2" \
  "rm b.txt" \
  "df" > "$TMP/cmds.txt"

boot "$TMP/df.img" "$TMP/s.txt" "$TMP/k.sock"
send_cmds "$TMP/k.sock" "$TMP/cmds.txt" 1.5
wait_count "$TMP/s.txt" "df: tombstones" 1 90 \
  || fail "guest never printed the df report"
shutdown

tr -d '\r' < "$TMP/s.txt" > "$TMP/s_clean.txt"
DF_CAP=$(grep -a "df: capacity" "$TMP/s_clean.txt" | tail -1)
DF_LIVE=$(grep -a "df: live" "$TMP/s_clean.txt" | tail -1)
DF_TOMB=$(grep -a "df: tombstones" "$TMP/s_clean.txt" | tail -1)
[ -n "$DF_CAP" ] && [ -n "$DF_LIVE" ] && [ -n "$DF_TOMB" ] \
  || fail "df lines missing from the serial log"
d_cap=$(echo "$DF_CAP" | sed -E 's/.*capacity ([0-9]+) sectors, free ([0-9]+).*/\1/')
d_free=$(echo "$DF_CAP" | sed -E 's/.*capacity ([0-9]+) sectors, free ([0-9]+).*/\2/')
d_lrec=$(echo "$DF_LIVE" | sed -E 's/.*live ([0-9]+) records, ([0-9]+) sectors.*/\1/')
d_lsec=$(echo "$DF_LIVE" | sed -E 's/.*live ([0-9]+) records, ([0-9]+) sectors.*/\2/')
d_trec=$(echo "$DF_TOMB" | sed -E 's/.*tombstones ([0-9]+) records, ([0-9]+) sectors.*/\1/')
d_tsec=$(echo "$DF_TOMB" | sed -E 's/.*tombstones ([0-9]+) records, ([0-9]+) sectors.*/\2/')

# Independent oracle: walk the image exactly like fsck_myfs.py
# (superblock count, 80-byte headers, tombstone flag at byte
# 72, extents of (80 + size + 511) / 512 sectors), extended to
# sector totals. Latest record per name wins.
ORACLE="$(python3 - "$TMP/df.img" << 'PYEOF'
import struct, sys
img = open(sys.argv[1], "rb").read()
total_sectors = len(img) // 512
magic, count = struct.unpack_from("<II", img, 0)
assert magic == 0x4D594653, "bad magic"
limit = total_sectors - 2048 if total_sectors > 3072 else total_sectors
lba = 1
latest = {}   # name -> (flagged, sectors)
walked = flagged_records = flagged_sectors = 0
for _ in range(count):
    off = lba * 512
    name = img[off:off + 64].split(b"\x00")[0].decode()
    size = struct.unpack_from("<Q", img, off + 64)[0]
    flagged = img[off + 72] & 1
    sects = (80 + size + 511) // 512
    walked += 1
    if flagged:
        flagged_records += 1
        flagged_sectors += sects
    latest[name] = (flagged, sects)
    lba += sects
live_records = sum(1 for f, _ in latest.values() if not f)
live_sectors = sum(s for f, s in latest.values() if not f)
used = lba - 1
print(f"records={walked} live={live_records} tombstones={flagged_records} "
      f"live_sectors={live_sectors} dead_sectors={used - live_sectors} "
      f"capacity={limit - 1} free={limit - lba}")
PYEOF
)" || fail "oracle walk failed"
eval "$ORACLE"
[ "$tombstones" -ge 1 ] || fail "oracle state has no tombstone (test construction broken): $ORACLE"

# fsck's own summary for the same image.
OUT="$(python3 "$ROOT/scripts/fsck_myfs.py" "$TMP/df.img")" \
  || fail "image not fsck-CLEAN: $OUT"
echo "$OUT" | grep -q "$records records walked, $live live files, $tombstones tombstones" \
  || fail "oracle walk disagrees with fsck: fsck=[$OUT] oracle=[$ORACLE]"
echo "[test] fsck: $OUT"

# Every df figure must match the oracle.
[ "$d_cap" = "$capacity" ]       || fail "df capacity $d_cap != oracle $capacity"
[ "$d_free" = "$free" ]           || fail "df free $d_free != oracle $free"
[ "$d_lrec" = "$live" ]           || fail "df live records $d_lrec != oracle $live"
[ "$d_lsec" = "$live_sectors" ]   || fail "df live sectors $d_lsec != oracle $live_sectors"
[ "$d_trec" = "$tombstones" ]     || fail "df tombstone records $d_trec != oracle/fsck $tombstones"
[ "$d_tsec" = "$dead_sectors" ]   || fail "df tombstone sectors $d_tsec != oracle $dead_sectors"
[ $((d_lrec + d_trec)) = "$records" ] || fail "df live+tombstones != records walked $records"

echo "[test] df agrees with fsck oracle: $DF_CAP / $DF_LIVE / $DF_TOMB"
echo "[test] PASS: shell df reports capacity/free/live/tombstone figures matching fsck on the same image (incl. non-tail tombstones)"
cleanup
exit 0
