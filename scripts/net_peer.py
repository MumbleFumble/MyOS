#!/usr/bin/env python3
"""Host-side peer for test_net.sh (Phase 7 inc.1).

QEMU's socket netdev in TCP stream mode (listen=127.0.0.1:10000)
frames each packet with a 4-byte big-endian length prefix. (UDP
mode is unusable in this sandbox: sendto() gets EPERM.)
Protocol:
  1. Connect to QEMU (retrying until it listens).
  2. Wait for the guest's hello frame (shell `netsend`): broadcast,
     ethertype 0x88B5, payload starting "MYOSNET-HELLO".
  3. Reply with a unicast frame to the guest MAC carrying
     "HOST-TO-MYOS-42"; the guest's net task logs it to serial,
     which the harness greps for.
Writes PEER OK / PEER FAIL: <reason> to the result file.
"""
import socket
import struct
import sys
import time

RESULT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/net_peer_result.txt"
GUEST_MAC = bytes.fromhex("525400123456")
DEADLINE = time.time() + 120


def done(msg):
    with open(RESULT, "w") as f:
        f.write(msg + "\n")
    sys.exit(0 if msg.startswith("PEER OK") else 1)


s = None
while time.time() < DEADLINE:
    try:
        s = socket.create_connection(("127.0.0.1", 10000), timeout=5)
        break
    except OSError:
        time.sleep(1)
if s is None:
    done("PEER FAIL: could not connect to QEMU socket")
s.settimeout(5)


def read_exact(n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("stream closed")
        buf += chunk
    return buf


def read_frame():
    (ln,) = struct.unpack(">I", read_exact(4))
    if ln > 4096:
        raise ValueError(f"bad frame length {ln}")
    return read_exact(ln)


def send_frame(frame):
    s.sendall(struct.pack(">I", len(frame)) + frame)


hello = None
while time.time() < DEADLINE:
    try:
        data = read_frame()
    except (socket.timeout, ConnectionError, ValueError):
        continue
    if len(data) >= 14 + 13 and data[12:14] == b"\x88\xb5" \
            and data[14:27] == b"MYOSNET-HELLO":
        hello = data
        break
if hello is None:
    done("PEER FAIL: no guest hello frame")
if hello[0:6] != b"\xff" * 6:
    done("PEER FAIL: hello not broadcast")
if hello[6:12] != GUEST_MAC:
    done("PEER FAIL: hello src is not the guest MAC")

payload = b"HOST-TO-MYOS-42"
frame = GUEST_MAC + bytes.fromhex("020000000099") + b"\x88\xb5" + payload
frame = frame.ljust(60, b"\x00")
send_frame(frame)
done("PEER OK")
