#!/usr/bin/env python3
"""Host-side peer for test_pingcmd.sh (Phase 7 inc.3).

Reactive peer for the guest's own `ping 192.168.77.1` shell command:
  - Answers the guest's broadcast ARP request for 192.168.77.1.
  - Answers each ICMP echo request with a matching echo reply
    (same id/seq/payload, valid checksums).
After answering 4 echo requests it writes PEER OK.
"""
import socket
import struct
import sys
import time

RESULT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/pingcmd_peer_result.txt"
GUEST_MAC = bytes.fromhex("525400123456")
PEER_MAC = bytes.fromhex("020000000099")
GUEST_IP = bytes([192, 168, 77, 2])
PEER_IP = bytes([192, 168, 77, 1])
DEADLINE = time.time() + 150


def done(msg):
    with open(RESULT, "w") as f:
        f.write(msg + "\n")
    sys.exit(0 if msg.startswith("PEER OK") else 1)


def cksum(data):
    if len(data) % 2:
        data += b"\x00"
    total = sum(struct.unpack(">%dH" % (len(data) // 2), data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


s = None
while time.time() < DEADLINE:
    try:
        s = socket.create_connection(("127.0.0.1", 10000), timeout=5)
        break
    except OSError:
        time.sleep(1)
if s is None:
    done("PEER FAIL: could not connect to QEMU socket")
s.settimeout(0.5)
rxbuf = b""


def poll_frames():
    global rxbuf
    frames = []
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            rxbuf += chunk
    except socket.timeout:
        pass
    while len(rxbuf) >= 4:
        (ln,) = struct.unpack(">I", rxbuf[:4])
        if len(rxbuf) < 4 + ln:
            break
        frames.append(rxbuf[4:4 + ln])
        rxbuf = rxbuf[4 + ln:]
    return frames


def send_frame(frame):
    s.sendall(struct.pack(">I", len(frame)) + frame)


def handle(fr):
    """React to one guest frame; return 1 if it was an echo request."""
    if len(fr) < 14:
        return 0
    etype = fr[12:14]
    if etype == b"\x08\x06" and len(fr) >= 42:
        a = fr[14:42]
        op = struct.unpack(">H", a[6:8])[0]
        if op == 1 and a[24:28] == PEER_IP and a[14:18] == GUEST_IP:
            reply = (
                GUEST_MAC + PEER_MAC + b"\x08\x06"
                + struct.pack(">HHBBH", 1, 0x0800, 6, 4, 2)
                + PEER_MAC + PEER_IP + GUEST_MAC + GUEST_IP
            ).ljust(60, b"\x00")
            send_frame(reply)
        return 0
    if etype == b"\x08\x00" and len(fr) >= 14 + 20 + 8:
        pkt = fr[14:]
        ihl = (pkt[0] & 0x0F) * 4
        tot = struct.unpack(">H", pkt[2:4])[0]
        if pkt[9] != 1 or pkt[16:20] != PEER_IP:
            return 0
        ic = pkt[ihl:tot]
        if ic[0] != 8:
            return 0
        ric = bytearray(ic)
        ric[0] = 0
        ric[2] = ric[3] = 0
        ric[2:4] = struct.pack(">H", cksum(bytes(ric)))
        total = 20 + len(ric)
        ip = struct.pack(">BBHHH", 0x45, 0, total, 0x2222, 0) \
            + struct.pack(">BB", 64, 1) + b"\x00\x00" + PEER_IP + GUEST_IP
        ip = ip[:10] + struct.pack(">H", cksum(ip)) + ip[12:]
        reply = (GUEST_MAC + PEER_MAC + b"\x08\x00" + ip + bytes(ric))
        send_frame(reply.ljust(60, b"\x00"))
        return 1
    return 0


answered = 0
while time.time() < DEADLINE and answered < 4:
    for fr in poll_frames():
        answered += handle(fr)
    time.sleep(0.05)

if answered >= 4:
    done("PEER OK")
done(f"PEER FAIL: answered only {answered} echo requests")
