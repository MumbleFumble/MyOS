#!/usr/bin/env bash
# Phase 15 inc.1: TCP fault-injection baseline. For each of the
# four deterministic fault profiles in tcp_fault_peer.py, boot
# the guest, run one tcpecho exchange through the fault, and
# assert the behaviour the CURRENT stack provably shows:
#   drop-first-data  completes after the guest retransmits
#   drop-first-ack   completes after the guest retransmits;
#                    the stored data is echoed exactly once
#   delay-echo       echo delayed 2.5s past the ~1s retransmit
#                    threshold; the guest's spurious retransmit
#                    is deduped and the transfer completes
#   ooo              the guest dup-ACKs and drops the early
#                    segment; completes after the peer resends
#                    it in order
# Every profile is bounded: the peer has a 120 s deadline and
# each guest wait 60 s, so no profile can hang the suite.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
DISK="$ROOT/build/disk.img"

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }

run_profile() {
  local prof="$1" faultmark="$2" evidmark="$3"
  local SERIAL=/tmp/myos_tcpfault_${prof}_serial.txt
  local SOCK=/tmp/qemu_tcpfault_${prof}_mon.sock
  local PEERLOG=/tmp/myos_tcpfault_${prof}_peer.log
  rm -f "$SERIAL" "$SOCK" "$PEERLOG"
  python3 "$ROOT/scripts/tcp_fault_peer.py" "$prof" > "$PEERLOG" 2>&1 &
  local PPEER=$!
  sleep 0.5
  qemu-system-x86_64 -m 256 -display none -vga none \
    -serial "file:$SERIAL" -monitor "unix:$SOCK,server,nowait" \
    -boot d -cdrom "$ISO" -drive "file=$DISK,format=raw,if=ide,index=0" \
    -netdev "socket,id=n0,connect=127.0.0.1:10000" \
    -device "e1000,netdev=n0,mac=52:54:00:12:34:56" &
  local QPID=$!
  local failmsg=""
  local i c k
  for i in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.2; done
  [ -S "$SOCK" ] || failmsg="monitor socket never appeared"
  if [ -z "$failmsg" ]; then
    sleep 20
    grep -a -q "e1000 network up" "$SERIAL" || failmsg="e1000 did not come up"
  fi
  if [ -z "$failmsg" ]; then
    local w="tcpecho 192.168.77.1 8080 hello-myos"
    for ((i=0; i<${#w}; i++)); do
      c="${w:i:1}"
      case "$c" in
        [a-z0-9]) k="$c" ;;
        ' ') k="spc" ;;
        '.') k="dot" ;;
        '-') k="minus" ;;
        *) failmsg="no key mapping for '$c'"; break ;;
      esac
      printf 'sendkey %s\n' "$k" | socat - "UNIX-CONNECT:$SOCK" >/dev/null
      sleep 0.06
    done
    printf 'sendkey ret\n' | socat - "UNIX-CONNECT:$SOCK" >/dev/null
  fi
  if [ -z "$failmsg" ]; then
    local t=0 n=0
    while [ "$t" -lt 60 ]; do
      n=$(grep -a -c "HELLO-TCP hello-myos" "$SERIAL" 2>/dev/null || true)
      [ "${n:-0}" -ge 1 ] && break
      sleep 1; t=$((t+1))
    done
    [ "${n:-0}" -ge 1 ] || failmsg="guest did not print greeting + echo within 60s"
  fi
  sleep 1
  if [ -z "$failmsg" ]; then
    grep -a -q "PEER OK" "$PEERLOG" || failmsg="peer did not validate the exchange"
  fi
  if [ -z "$failmsg" ]; then
    grep -a -q "$faultmark" "$PEERLOG" || failmsg="fault was never injected ($faultmark missing)"
  fi
  if [ -z "$failmsg" ]; then
    grep -a -q "$evidmark" "$PEERLOG" || failmsg="expected evidence missing ($evidmark)"
  fi
  kill -9 "$QPID" "$PPEER" 2>/dev/null
  wait "$QPID" 2>/dev/null; wait "$PPEER" 2>/dev/null
  if [ -n "$failmsg" ]; then
    echo "[test] FAIL ($prof): $failmsg"
    echo "--- peer log ($prof) ---"; cat "$PEERLOG"
    echo "--- serial tail ($prof) ---"; tail -15 "$SERIAL" 2>/dev/null
    exit 1
  fi
  echo "[test] profile $prof: PASS (fault injected, evidenced, transfer completed)"
}

run_profile drop-first-data "FAULT drop-first-data" "GUEST RETRANSMIT"
run_profile drop-first-ack  "FAULT drop-first-ack"  "GUEST RETRANSMIT"
run_profile delay-echo      "FAULT delay-echo"      "GUEST RETRANSMIT"
run_profile ooo             "FAULT ooo"             "GUEST DUP-ACK"
run_profile drop-first-two-data "FAULT drop-first-two-data" "BACKOFF gap1="

# Backoff inequality (Phase 15 inc.2): the second retransmit gap
# must be at least 1.8x the first, from the peer's timestamps,
# AND the absolute gaps must sit at the computed RTO (20-tick
# floor = 200 ms, doubled to 400 ms) — a run where no RTT sample
# was ever taken would keep the old 100-tick initial RTO and
# produce ~1 s / ~2 s gaps at the same 2.0 ratio.
python3 - << 'EOF' || { echo "[test] FAIL: backoff inequality not satisfied"; exit 1; }
import re
log = open("/tmp/myos_tcpfault_drop-first-two-data_peer.log").read()
m = re.search(r"BACKOFF gap1=([\d.]+)s gap2=([\d.]+)s ratio=([\d.]+)", log)
assert m, "BACKOFF line missing"
g1, g2, ratio = float(m.group(1)), float(m.group(2)), float(m.group(3))
print(f"[test] backoff evidence: gap1={g1:.3f}s gap2={g2:.3f}s ratio={ratio:.2f}")
assert ratio >= 1.8, f"ratio {ratio:.2f} < 1.8"
assert 0.150 <= g1 <= 0.280, f"gap1 {g1:.3f}s not at the 200 ms floored RTO"
assert 0.340 <= g2 <= 0.560, f"gap2 {g2:.3f}s not at the doubled 400 ms RTO"
EOF

# Reno profile (Phase 15 inc.5): different shape from the five
# above — QEMU listens, the fault peer connects as a TCP client,
# and the guest runs tcpechosrv, so the GUEST is the sender whose
# congestion control is under test. Evidence is the peer's RENO
# lines plus the kernel's own [tcp] serial lines, whose old->new
# values pin the whole cwnd/ssthresh evolution: slow start
# 1024 -> 5120 over the handshake + three echo ACKs, fast
# retransmit halving to 2560, one congestion-avoidance step to
# 2969, and the timeout collapse to cwnd 1024 / ssthresh 2048.
run_reno() {
  local SERIAL=/tmp/myos_tcpfault_reno_serial.txt
  local SOCK=/tmp/qemu_tcpfault_reno_mon.sock
  local PEERLOG=/tmp/myos_tcpfault_reno_peer.log
  rm -f "$SERIAL" "$SOCK" "$PEERLOG"
  qemu-system-x86_64 -m 256 -display none -vga none \
    -serial "file:$SERIAL" -monitor "unix:$SOCK,server,nowait" \
    -boot d -cdrom "$ISO" -drive "file=$DISK,format=raw,if=ide,index=0" \
    -netdev "socket,id=n0,listen=127.0.0.1:10000" \
    -device "e1000,netdev=n0,mac=52:54:00:12:34:56" &
  local QPID=$!
  sleep 1
  python3 "$ROOT/scripts/tcp_fault_peer.py" reno > "$PEERLOG" 2>&1 &
  local PPEER=$!
  local failmsg=""
  local i c k
  for i in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.2; done
  [ -S "$SOCK" ] || failmsg="monitor socket never appeared"
  if [ -z "$failmsg" ]; then
    sleep 20
    grep -a -q "e1000 network up" "$SERIAL" || failmsg="e1000 did not come up"
  fi
  if [ -z "$failmsg" ]; then
    local w="tcpechosrv 8080"
    for ((i=0; i<${#w}; i++)); do
      c="${w:i:1}"
      case "$c" in
        [a-z0-9]) k="$c" ;;
        ' ') k="spc" ;;
        *) failmsg="no key mapping for '$c'"; break ;;
      esac
      printf 'sendkey %s\n' "$k" | socat - "UNIX-CONNECT:$SOCK" >/dev/null
      sleep 0.06
    done
    printf 'sendkey ret\n' | socat - "UNIX-CONNECT:$SOCK" >/dev/null
  fi
  if [ -z "$failmsg" ]; then
    local t=0
    while [ "$t" -lt 150 ]; do
      grep -a -q "PEER OK" "$PEERLOG" 2>/dev/null && break
      grep -a -q "PEER FAIL" "$PEERLOG" 2>/dev/null && break
      sleep 1; t=$((t+1))
    done
    grep -a -q "PEER OK" "$PEERLOG" || failmsg="peer did not validate the Reno exchange"
  fi
  sleep 1
  if [ -z "$failmsg" ]; then
    grep -a -q "RENO fast-retransmit dt=" "$PEERLOG" || failmsg="fast-retransmit timing evidence missing"
  fi
  if [ -z "$failmsg" ]; then
    grep -a -q "RENO timeout gaps" "$PEERLOG" || failmsg="timeout gap evidence missing"
  fi
  if [ -z "$failmsg" ]; then
    grep -a -q "tcpechosrv: closed" "$SERIAL" || failmsg="guest server did not close cleanly"
  fi
  if [ -z "$failmsg" ]; then
    local nfr ntm lfr ltm
    nfr=$(grep -a -c "fast-retransmit cwnd=5120->2560 ssthresh=65535->2560" "$SERIAL" || true)
    [ "${nfr:-0}" -eq 1 ] || failmsg="fast-retransmit serial line missing/duplicated (n=$nfr)"
    ntm=$(grep -a -c "timeout cwnd=2969->1024 ssthresh=2560->2048" "$SERIAL" || true)
    [ "${ntm:-0}" -eq 1 ] || failmsg="timeout serial line missing/duplicated (n=$ntm)"
    if [ -z "$failmsg" ]; then
      [ "$(grep -a -c '\[tcp\] timeout' "$SERIAL" || true)" -eq 1 ] || failmsg="more than one [tcp] timeout line (second timeout must not re-collapse)"
    fi
    if [ -z "$failmsg" ]; then
      lfr=$(grep -a -n "fast-retransmit" "$SERIAL" | head -1 | cut -d: -f1)
      ltm=$(grep -a -n "\[tcp\] timeout" "$SERIAL" | head -1 | cut -d: -f1)
      [ "$lfr" -lt "$ltm" ] || failmsg="fast retransmit did not precede the timeout"
    fi
  fi
  kill -9 "$QPID" "$PPEER" 2>/dev/null
  wait "$QPID" 2>/dev/null; wait "$PPEER" 2>/dev/null
  if [ -n "$failmsg" ]; then
    echo "[test] FAIL (reno): $failmsg"
    echo "--- peer log (reno) ---"; cat "$PEERLOG"
    echo "--- serial tail (reno) ---"; tail -20 "$SERIAL" 2>/dev/null
    exit 1
  fi
  echo "[test] profile reno: PASS (fast retransmit on 3rd dup ACK, cwnd/ssthresh evolution and timeout collapse evidenced)"
}

run_reno

echo "[test] PASS: all fault profiles injected deterministically and the current stack's behaviour under each is as characterised"
