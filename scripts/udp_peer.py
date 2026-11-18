#!/usr/bin/env python3
"""Host-side peer for test_udp.sh (Phase 7 inc.3).

Sends a UDP datagram from 192.168.77.1:5000 to the guest's echo
server at 192.168.77.2:7 with payload "MYOSUDP42" and expects the
same payload back from port 7, ports swapped, IP checksum valid.
The request is retried every 3 s while the guest boots.
Writes PEER OK / PEER FAIL: <reason> to the result file.
"""
import socket
import struct
import sys
import time

RESULT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/udp_peer_result.txt"
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


def wait_for(pred, timeout_s):
    end = time.time() + timeout_s
    while time.time() < end:
        for fr in poll_frames():
            if pred(fr):
                return fr
        time.sleep(0.1)
    return None


payload = b"MYOSUDP42"
udp = struct.pack(">HHHH", 5000, 7, 8 + len(payload), 0) + payload
total = 20 + len(udp)
ip = struct.pack(">BBHHH", 0x45, 0, total, 0x1234, 0) \
    + struct.pack(">BB", 64, 17) + b"\x00\x00" + PEER_IP + GUEST_IP
ip = ip[:10] + struct.pack(">H", cksum(ip)) + ip[12:]
request = (GUEST_MAC + PEER_MAC + struct.pack(">H", 0x0800) + ip + udp)
request = request.ljust(60, b"\x00")


def is_echo_reply(fr):
    if len(fr) < 14 + 20 + 8 or fr[12:14] != b"\x08\x00":
        return False
    if fr[0:6] != PEER_MAC or fr[6:12] != GUEST_MAC:
        return False
    pkt = fr[14:]
    ihl = (pkt[0] & 0x0F) * 4
    if pkt[12:16] != GUEST_IP or pkt[16:20] != PEER_IP or pkt[9] != 17:
        return False
    if cksum(pkt[:ihl]) != 0:
        return False
    u = pkt[ihl:]
    sport, dport, ulen = struct.unpack(">HHH", u[:6])
    return sport == 7 and dport == 5000 and u[8:ulen] == payload


reply = None
while time.time() < DEADLINE and reply is None:
    send_frame(request)
    reply = wait_for(is_echo_reply, 3)
if reply is None:
    done("PEER FAIL: no UDP echo reply from guest")

done("PEER OK")
