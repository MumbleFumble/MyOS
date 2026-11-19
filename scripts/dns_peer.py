#!/usr/bin/env python3
"""DNS peer for scripts/test_dns.sh (socket backend, TCP stream mode).

Validates the guest's A query for example.com (id echoed, RD flag,
label encoding, QTYPE/QCLASS) and replies with one A record,
203.0.113.77 (TEST-NET-3), using a compression pointer for the name.
Also answers ARP requests so the guest can reach us. Prints
PEER OK / PEER FAIL: <reason>.
"""
import socket
import struct
import sys
import time

GUEST_IP = bytes([192, 168, 77, 2])
PEER_IP = bytes([192, 168, 77, 1])
GUEST_MAC = bytes.fromhex("525400123456")
PEER_MAC = bytes.fromhex("020000000099")
ANSWER_IP = bytes([203, 0, 113, 77])
DEADLINE = time.time() + 150


def cksum(data):
    if len(data) % 2:
        data += b"\x00"
    s = sum(struct.unpack(">%dH" % (len(data) // 2), data))
    s = (s >> 16) + (s & 0xFFFF)
    s += s >> 16
    return (~s) & 0xFFFF


def fail(msg):
    print(f"PEER FAIL: {msg}", flush=True)
    sys.exit(1)


srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 10000))
srv.listen(1)
conn, _ = srv.accept()
conn.settimeout(5)
buf = b""


def poll_frames():
    global buf
    try:
        data = conn.recv(65536)
        if data:
            buf += data
    except (socket.timeout, TimeoutError):
        pass
    frames = []
    while len(buf) >= 4:
        (ln,) = struct.unpack(">I", buf[:4])
        if len(buf) < 4 + ln:
            break
        frames.append(buf[4:4 + ln])
        buf = buf[4 + ln:]
    return frames


def send_frame(fr):
    conn.sendall(struct.pack(">I", len(fr)) + fr)


def udp_packet(dst_mac, src_ip, dst_ip, sport, dport, payload):
    ulen = 8 + len(payload)
    udp = struct.pack(">HHHH", sport, dport, ulen, 0) + payload
    total = 20 + ulen
    ip = struct.pack(">BBHHH", 0x45, 0, total, 0x4444, 0) \
        + struct.pack(">BB", 64, 17) + b"\x00\x00" + src_ip + dst_ip
    ip = ip[:10] + struct.pack(">H", cksum(ip)) + ip[12:]
    return dst_mac + PEER_MAC + b"\x08\x00" + ip + udp


def handle(fr):
    if len(fr) < 14:
        return
    etype = fr[12:14]
    if etype == b"\x08\x06" and len(fr) >= 42:
        arp = fr[14:42]
        if arp[6:8] == b"\x00\x01" and arp[24:28] == PEER_IP:
            reply = struct.pack(">HHBBH", 1, 0x0800, 6, 4, 2) \
                + PEER_MAC + PEER_IP + arp[8:14] + arp[14:18]
            send_frame((arp[8:14] + PEER_MAC + b"\x08\x06" + reply).ljust(60, b"\x00"))
        return
    if etype != b"\x08\x00" or len(fr) < 14 + 20 + 8:
        return
    pkt = fr[14:]
    ihl = (pkt[0] & 0x0F) * 4
    tot = struct.unpack(">H", pkt[2:4])[0]
    if pkt[9] != 17 or pkt[16:20] != PEER_IP:
        return
    udp = pkt[ihl:tot]
    sport, dport = struct.unpack(">HH", udp[:4])
    if dport != 53:
        return
    q = udp[8:]
    if len(q) < 12 + 16:
        fail("short DNS query")
    qid, flags, qd = struct.unpack(">HHH", q[:6])
    if flags & 0x0100 == 0:
        fail("RD flag not set")
    if qd != 1:
        fail(f"QDCOUNT {qd}")
    # Question name must be example.com, type A, class IN.
    if q[12] != 7 or q[13:20] != b"example" or q[20] != 3 \
            or q[21:24] != b"com" or q[24] != 0:
        fail("question name is not example.com")
    if q[25:29] != b"\x00\x01\x00\x01":
        fail("question is not A/IN")
    question = q[12:29]
    answer = struct.pack(">H", 0xC00C) \
        + struct.pack(">HHIH", 1, 1, 60, 4) + ANSWER_IP
    resp = struct.pack(">HHHHHH", qid, 0x8180, 1, 1, 0, 0) + question + answer
    src_mac = fr[6:12]
    send_frame(udp_packet(src_mac, PEER_IP, GUEST_IP, 53, sport, resp))
    print("PEER OK", flush=True)
    sys.exit(0)


while time.time() < DEADLINE:
    for fr in poll_frames():
        handle(fr)
    time.sleep(0.05)
fail("no DNS query received")
