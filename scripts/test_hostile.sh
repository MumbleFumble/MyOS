#!/usr/bin/env bash
# Phase 17 inc.8: hostile-set containment. Runs the
# hostile cases assembled by inc-002 (fault path),
# inc-003 (copy validation) and inc-004 (ELF hardening),
# plus bad syscall number, deep recursion and an
# infinite loop, and proves the shell alive with exact
# output after EACH case. Final check: the inc-007
# pipeline cat.elf | wc.elf producing its exact line
# while the infinite-loop spinner is STILL running.
# Fixtures are reused, not modified: faulttest/faultprobe,
# badptr, and scripts/make_badelf.py's four classes.
set -u
cd "$(dirname "$0")/.."
IMG=build/disk.img
[ -f "$IMG" ] || { echo "[test] FAIL: $IMG missing"; exit 1; }

WORK=$(mktemp -d)
cp "$IMG" "$WORK/disk.img"
python3 scripts/make_badelf.py build/user/argtest.elf "$WORK/disk.img" > /dev/null \
  || { echo "[test] FAIL: make_badelf.py failed"; exit 1; }
# ep_big.txt fixture for the final pipeline (200 lines
# of "word word\n" = 2000 bytes), appended after the
# badelf records.
python3 - "$WORK/disk.img" << 'EOF'
import struct, sys
path = sys.argv[1]
img = bytearray(open(path, 'rb').read())
magic, count = struct.unpack_from('<II', img, 0)
assert magic == 0x4D594653
off = 512
for _ in range(count):
    size = struct.unpack_from('<Q', img, off + 64)[0]
    off += ((80 + size + 511) // 512) * 512
data = b"word word\n" * 200
hdr = bytearray(80)
hdr[0:10] = b"ep_big.txt"
struct.pack_into('<Q', hdr, 64, len(data))
rec = bytes(hdr) + data
rec += b'\0' * (((len(rec) + 511) // 512) * 512 - len(rec))
img[off:off + len(rec)] = rec
struct.pack_into('<I', img, 4, count + 1)
open(path, 'wb').write(img)
EOF

SER="$WORK/serial.log"
MON="$WORK/mon.sock"
qemu-system-x86_64 -m 64 -display none -serial "file:$SER" \
  -monitor "unix:$MON,server,nowait" \
  -drive file="$WORK/disk.img",format=raw,if=ide,index=0 \
  -boot order=d,menu=off -cdrom build/myos.iso &
QPID=$!
cleanup() { kill $QPID 2>/dev/null; wait $QPID 2>/dev/null; }
trap cleanup EXIT

send() { echo "$1" | socat - "UNIX-CONNECT:$MON" >/dev/null 2>&1; }
count() { grep -o -F "$1" "$2" 2>/dev/null | wc -l; }

type_word() {
  local w="$1" i c k
  for (( i=0; i<${#w}; i++ )); do
    c="${w:i:1}"
    case "$c" in
      ' ') k="spc" ;;
      '.') k="dot" ;;
      '-') k="minus" ;;
      '/') k="slash" ;;
      '|') k="shift-backslash" ;;
      '_') k="shift-minus" ;;
      *)   k="$c" ;;
    esac
    send "sendkey $k"
    sleep 0.06
  done
  send "sendkey ret"
  sleep 0.6
}

wait_for_count() {
  local s="$1" want="$2" n=0
  for _ in $(seq 1 90); do
    n=$(count "$s" "$SER")
    [ "$n" -ge "$want" ] && return 0
    sleep 2
  done
  echo "[test] timed out waiting for '$s' >= $want (saw $n)"
  return 1
}

fail() { echo "[test] FAIL: $1"; tail -30 "$SER" 2>/dev/null; exit 1; }
check() { # check <string> <expected-count>
  local n
  n=$(count "$1" "$SER")
  [ "$n" -eq "$2" ] || fail "count of '$1' is $n, expected $2"
}
alive() { # alive <tag>: the shell must answer exactly
  type_word "frobnicate $1"
  wait_for_count "echo: frobnicate $1" 1 || fail "shell not alive after $1"
}

for _ in $(seq 1 30); do [ -S "$MON" ] && break; sleep 1; done
[ -S "$MON" ] || fail "monitor socket never appeared"
wait_for_count "MyOS" 1 || fail "kernel banner missing"
sleep 10

# Case 1: null deref (inc-002 kill path, EXIT_FAULT).
type_word "exec faulttest.elf null"
wait_for_count "faulttest: PASS null" 1 || fail "null deref not contained"
alive a1

# Case 2: divide by zero (inc-002 kill path).
type_word "exec faulttest.elf div0"
wait_for_count "faulttest: PASS div0" 1 || fail "div0 not contained"
alive a2

# Case 3: bad pointers to core syscalls (inc-003 EFAULT;
# the program itself survives by design).
type_word "exec badptr.elf"
wait_for_count "badptr: roundtrip OK" 1 || fail "badptr case failed"
alive a3

# Case 4: bad syscall numbers (dispatcher ENOSYS -1).
type_word "exec hostileprobe.elf badnr"
wait_for_count "hostileprobe: ok badnr" 1 || fail "badnr case failed"
alive a4

# Case 5: lying ELFs (inc-004 classes, refused whole).
type_word "exec badelf1.elf"
type_word "exec badelf2.elf"
type_word "exec badelf3.elf"
type_word "exec badelf4.elf"
wait_for_count "exec: failed" 4 || fail "badelf classes not all refused"
alive a5

# Case 6: deep recursion (stack overflow -> kill path).
type_word "exec hostileprobe.elf run-recurse"
wait_for_count "hostileprobe: ok recurse" 1 || fail "recursion not contained"
alive a6

# Case 7: infinite loop must not starve a second
# program. The spinner is left running for the rest of
# the test (no kill syscall exists; QEMU teardown ends
# it) - the final check below runs with it alive.
type_word "exec hostileprobe.elf run-spin"
wait_for_count "hostileprobe: ok spin" 1 || fail "spinner starved argtest"
alive a7

# Final correctness check, spinner still running.
type_word "cat.elf ep_big.txt | wc.elf"
wait_for_count "200 400 2000" 1 || fail "final pipeline wrong with spinner alive"
sleep 2

check "faulttest: PASS" 2
check "exit=4000" 2
check "badptr: ok " 10
check "badptr: FAIL" 0
check "hostileprobe: ok badnr" 1
check "exec: failed" 4
check "hostileprobe: ok recurse" 1
check "hostileprobe: ok spin" 1
check "hostileprobe: FAIL" 0
check "argc=3" 1
check "200 400 2000" 1
check "> " 19

echo "[test] PASS: hostile set contained (faults, EFAULT, bad nr, lying ELF, recursion, spinner), shell alive after each case"
