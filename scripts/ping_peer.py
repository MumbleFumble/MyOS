#!/usr/bin/env python3
"""Host-side peer for test_ping.sh (Phase 7 inc.2).

Plays the network over QEMU's TCP-stream socket netdev:
  1. ARP: broadcast a request for 192.168.77.2 (the guest), expect a
     unicast reply carrying the guest MAC.
  2. ICMP: send an echo request (id 0x4242, seq 7, payload
     "MYOSPING42") from 192.168.77.1, expect the echo reply with
     id/seq/payload intact and both checksums valid.
Requests are retried every 3 s (the guest takes time to boot).
Writes PEER OK / PEER FAIL: <reason> to the result file.
"""
import socket
import struct
import sys
import time

RESULT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/ping_peer_result.txt"
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
    """Drain the stream, return a list of complete frames."""
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


# ---- 1. ARP ----
arp_req = (
    b"\xff" * 6 + PEER_MAC + struct.pack(">H", 0x0806)
    + struct.pack(">HHBBH", 1, 0x0800, 6, 4, 1)
    + PEER_MAC + PEER_IP + b"\x00" * 6 + GUEST_IP
).ljust(60, b"\x00")


def is_arp_reply(fr):
    if len(fr) < 42 or fr[12:14] != b"\x08\x06":
        return False
    a = fr[14:42]
    htype, ptype, hlen, plen, op = struct.unpack(">HHBBH", a[:8])
    return (
        htype == 1 and ptype == 0x0800 and hlen == 6 and plen == 4
        and op == 2
        and a[8:14] == GUEST_MAC and a[14:18] == GUEST_IP
        and a[18:24] == PEER_MAC and a[24:28] == PEER_IP
        and fr[0:6] == PEER_MAC and fr[6:12] == GUEST_MAC
    )


reply = None
while time.time() < DEADLINE and reply is None:
    send_frame(arp_req)
    reply = wait_for(is_arp_reply, 3)
if reply is None:
    done("PEER FAIL: no ARP reply from guest")

# ---- 2. ICMP echo ----
payload = b"MYOSPING42"
icmp = struct.pack(">BBHHH", 8, 0, 0, 0x4242, 7) + payload
icmp = struct.pack(">BBHHH", 8, 0, cksum(icmp), 0x4242, 7) + payload
total = 20 + len(icmp)
ip = struct.pack(">BBHHH", 0x45, 0, total, 0x1234, 0) \
    + struct.pack(">BB", 64, 1) + b"\x00\x00" + PEER_IP + GUEST_IP
ip = ip[:10] + struct.pack(">H", cksum(ip)) + ip[12:]
echo_req = (GUEST_MAC + PEER_MAC + struct.pack(">H", 0x0800) + ip + icmp)
echo_req = echo_req.ljust(60, b"\x00")


def is_echo_reply(fr):
    if len(fr) < 14 + 20 + 8 or fr[12:14] != b"\x08\x00":
        return False
    if fr[0:6] != PEER_MAC or fr[6:12] != GUEST_MAC:
        return False
    pkt = fr[14:]
    ihl = (pkt[0] & 0x0F) * 4
    tot = struct.unpack(">H", pkt[2:4])[0]
    if pkt[12:16] != GUEST_IP or pkt[16:20] != PEER_IP or pkt[9] != 1:
        return False
    if cksum(pkt[:ihl]) != 0:
        return False
    ic = pkt[ihl:tot]
    if cksum(ic) != 0:
        return False
    typ, code, _, ident, seq = struct.unpack(">BBHHH", ic[:8])
    return typ == 0 and code == 0 and ident == 0x4242 and seq == 7 \
        and ic[8:] == payload


reply = None
end2 = time.time() + 60
while time.time() < end2 and reply is None:
    send_frame(echo_req)
    reply = wait_for(is_echo_reply, 3)
if reply is None:
    done("PEER FAIL: no ICMP echo reply from guest")

done("PEER OK")
