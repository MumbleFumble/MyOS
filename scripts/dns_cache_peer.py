#!/usr/bin/env python3
"""Host-side DNS server for test_dns_cache.sh (Phase 11 inc.3).

Answers A queries over QEMU's TCP-stream socket netdev:
  example.com -> 203.0.113.77, TTL 12 (expires during the test)
  other.com   -> 203.0.113.78, TTL 60
Every query's name is appended to the log file (argv[2]) so the
test can count how many queries actually reached the server:
a repeat lookup served from the guest's cache sends none.
Also answers ARP for 192.168.77.1 (the resolver ARPs first).
Runs until killed.
"""
import socket
import struct
import sys

LOG = sys.argv[2] if len(sys.argv) > 2 else "/tmp/dns_cache_queries.log"
GUEST_MAC = bytes.fromhex("525400123456")
PEER_MAC = bytes.fromhex("020000000099")
GUEST_IP = bytes([192, 168, 77, 2])
PEER_IP = bytes([192, 168, 77, 1])
ANSWERS = {
    "example.com": (bytes([203, 0, 113, 77]), 12),
    "other.com": (bytes([203, 0, 113, 78]), 60),
}


def cksum(data):
    if len(data) % 2:
        data += b"\x00"
    total = sum(struct.unpack(">%dH" % (len(data) // 2), data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


s = None
import time
while True:
    try:
        s = socket.create_connection(("127.0.0.1", 10000), timeout=5)
        break
    except OSError:
        time.sleep(1)
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
            while len(rxbuf) >= 4:
                n = struct.unpack(">I", rxbuf[:4])[0]
                if len(rxbuf) < 4 + n:
                    break
                frames.append(rxbuf[4:4 + n])
                rxbuf = rxbuf[4 + n:]
            if len(chunk) < 65536:
                break
    except (socket.timeout, BlockingIOError):
        pass
    return frames


def send(frame):
    s.sendall(struct.pack(">I", len(frame)) + frame)


def udp_packet(sport, dport, payload):
    ulen = 8 + len(payload)
    udp = struct.pack(">HHHH", sport, dport, ulen, 0) + payload
    total = 20 + ulen
    ip = struct.pack(">BBHHH", 0x45, 0, total, 0x4444, 0) \
        + struct.pack(">BB", 64, 17) + b"\x00\x00" + PEER_IP + GUEST_IP
    ip = ip[:10] + struct.pack(">H", cksum(ip)) + ip[12:]
    return GUEST_MAC + PEER_MAC + b"\x08\x00" + ip + udp


def parse_qname(q):
    labels = []
    i = 12
    while i < len(q) and q[i] != 0:
        ln = q[i]
        labels.append(q[i + 1:i + 1 + ln].decode("ascii", "replace"))
        i += 1 + ln
    return ".".join(labels), i + 1  # offset just past the name


def handle(fr):
    if len(fr) < 14:
        return
    if fr[12:14] == b"\x08\x06" and len(fr) >= 42:
        arp = fr[14:42]
        if arp[6:8] == b"\x00\x01" and arp[24:28] == PEER_IP:
            reply = struct.pack(">HHBBH", 1, 0x0800, 6, 4, 2) \
                + PEER_MAC + PEER_IP + arp[8:14] + arp[14:18]
            send((arp[8:14] + PEER_MAC + b"\x08\x06" + reply).ljust(60, b"\x00"))
        return
    if fr[12:14] != b"\x08\x00" or len(fr) < 14 + 20 + 8:
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
    if len(q) < 12:
        return
    qid = struct.unpack(">H", q[:2])[0]
    name, qend = parse_qname(q)
    with open(LOG, "a") as f:
        f.write(name + "\n")
    if name not in ANSWERS:
        return
    ip, ttl = ANSWERS[name]
    question = q[12:qend + 4]
    resp = struct.pack(">HHHHHH", qid, 0x8180, 1, 1, 0, 0) + question
    resp += b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, ttl, 4) + ip
    send(udp_packet(53, sport, resp))


while True:
    for fr in poll_frames():
        handle(fr)
    time.sleep(0.05)
