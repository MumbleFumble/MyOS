#!/usr/bin/env python3
"""Fault-injecting TCP peer for scripts/test_tcpfault.sh
(Phase 15 inc.1: baseline characterisation, no kernel change).

Same wire role as tcp_peer.py (server on the tcpecho path:
greeting on connect, echo the guest's data, then close), plus a
named, deterministic fault profile applied to the data phase:

  drop-first-data  the guest's first data segment is treated as
                   never received (no ACK, no echo); the guest
                   must retransmit to make progress.
  drop-first-two-data  the first TWO arrivals of the guest's
                   data segment are dropped the same way, so the
                   transfer needs two retransmits; the peer logs
                   the two retransmit gaps (BACKOFF line) so the
                   test can assert exponential backoff on the wire.
  drop-first-ack   the guest's first data segment is stored but
                   its ACK is withheld (a dropped ACK is wire-
                   identical to one never sent); on the guest's
                   retransmission the stored data is echoed once.
  delay-echo       the echo is delayed 2.5 s, past the guest's
                   ~1 s retransmit threshold; the guest's spurious
                   retransmission is deduped (pure ACK), then the
                   delayed echo completes the transfer.
  ooo              the echo goes out as two segments, second half
                   first; the guest is expected to dup-ACK and
                   drop it, and the peer resends it in order once
                   the first half lands.

Every segment both ways is logged with a relative timestamp;
fault actions log a FAULT line; guest retransmissions and
dup-ACKs log dedicated lines the test asserts on. Writes
PEER OK / PEER FAIL to stdout (the test captures it).
"""
import socket
import struct
import sys
import time

PROFILE = sys.argv[1] if len(sys.argv) > 1 else "drop-first-data"
GUEST_IP = bytes([192, 168, 77, 2])
PEER_IP = bytes([192, 168, 77, 1])
PEER_MAC = bytes.fromhex("020000000099")
GUEST_MAC = bytes.fromhex("525400123456")
PORT = 8080
ISS = 1000
GREETING = b"HELLO-TCP "
EXPECT = b"hello-myos"
DELAY_S = 2.5
DEADLINE = time.time() + 120
T0 = time.time()

F_FIN, F_SYN, F_RST, F_PSH, F_ACK = 0x01, 0x02, 0x04, 0x08, 0x10
FLAGNAMES = [(F_FIN, "FIN"), (F_SYN, "SYN"), (F_RST, "RST"),
             (F_PSH, "PSH"), (F_ACK, "ACK")]


def log(msg):
    print(f"[{time.time() - T0:7.3f}] {msg}", flush=True)


def fname(flags):
    return "|".join(n for b, n in FLAGNAMES if flags & b) or "0"


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
conn.settimeout(0.2)   # short poll so the delay-echo timer is accurate
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


gport = None
gseq0 = None
nxt = ISS
fin_seq = None


def send_tcp(seq, ack, flags, payload=b"", note=""):
    global nxt
    seg = struct.pack(">HHII", PORT, gport, seq, ack) \
        + struct.pack(">BBHHH", 5 << 4, flags, 2048, 0, 0) + payload
    pseudo = PEER_IP + GUEST_IP + struct.pack(">BBH", 0, 6, len(seg))
    seg = seg[:16] + struct.pack(">H", cksum(pseudo + seg)) + seg[18:]
    total = 20 + len(seg)
    ip = struct.pack(">BBHHH", 0x45, 0, total, 0x5555, 0) \
        + struct.pack(">BB", 64, 6) + b"\x00\x00" + PEER_IP + GUEST_IP
    ip = ip[:10] + struct.pack(">H", cksum(ip)) + ip[12:]
    send_frame((GUEST_MAC + PEER_MAC + b"\x08\x00" + ip + seg).ljust(60, b"\x00"))
    log(f"-> seq={seq} ack={ack} {fname(flags)} len={len(payload)}"
        + (f" {payload!r}" if payload else "") + note)
    nxt = seq + len(payload) + (1 if flags & (F_SYN | F_FIN) else 0)


state = "listen"
data_seq = None        # seq of the guest's data segment
data_seen = 0          # payload-bearing guest segments received
data_times = []        # receive time of each data arrival
stored = None          # guest payload stored by the peer
echo_due = None        # delay-echo: when the echo may be sent
echo_sent = False
ooo_stage = 0          # ooo: 0 idle, 1 halves sent, 2 second half resent
guest_fin = False
acked_fin = False


def start_fin(ack):
    global fin_seq
    fin_seq = nxt
    send_tcp(nxt, ack, F_FIN | F_ACK)
    return "closing"


def handle(fr):
    global gport, gseq0, state, data_seq, data_seen, stored
    global echo_due, echo_sent, ooo_stage, guest_fin, acked_fin
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
    if etype != b"\x08\x00" or len(fr) < 14 + 20 + 20:
        return
    pkt = fr[14:]
    ihl = (pkt[0] & 0x0F) * 4
    tot = struct.unpack(">H", pkt[2:4])[0]
    if pkt[9] != 6 or pkt[16:20] != PEER_IP:
        return
    seg = pkt[ihl:tot]
    sport, dport, seq, ack = struct.unpack(">HHII", seg[:12])
    doff = (seg[12] >> 4) * 4
    flags = seg[13]
    payload = seg[doff:]
    if dport != PORT:
        return
    pseudo = GUEST_IP + PEER_IP + struct.pack(">BBH", 0, 6, len(seg))
    if cksum(pseudo + seg) != 0:
        fail("guest TCP checksum invalid")
    if state == "listen":
        if flags != F_SYN:
            fail(f"expected bare SYN, got flags {flags:#x}")
        gport, gseq0 = sport, seq
        log(f"<- seq={seq} ack={ack} {fname(flags)} (guest SYN, port {gport})")
        send_tcp(ISS, seq + 1, F_SYN | F_ACK)
        state = "syn"
        return
    if sport != gport:
        return
    log(f"<- seq={seq} ack={ack} {fname(flags)} len={len(payload)}"
        + (f" {payload!r}" if payload else ""))
    if state == "syn":
        if (flags & F_ACK) and ack == ISS + 1:
            state = "est"
            send_tcp(nxt, seq, F_PSH | F_ACK, GREETING)
        return
    # established / closing
    if payload:
        if payload != EXPECT:
            fail(f"data payload {payload!r} != {EXPECT!r}")
        if data_seq is None:
            data_seq = seq
        elif seq != data_seq:
            fail(f"unexpected data seq {seq}, expected {data_seq}")
        data_seen += 1
        data_times.append(time.time())
        if data_seen > 1:
            log(f"GUEST RETRANSMIT: data segment seen again "
                f"(seq={seq}, arrival #{data_seen})")
        if PROFILE == "drop-first-two-data" and data_seen <= 2:
            log(f"FAULT drop-first-two-data: guest data segment "
                f"arrival #{data_seen} (seq={seq} len={len(payload)}) "
                f"dropped (no ACK, no echo)")
            return
        if PROFILE == "drop-first-two-data" and data_seen == 3:
            g1 = data_times[1] - data_times[0]
            g2 = data_times[2] - data_times[1]
            log(f"BACKOFF gap1={g1:.3f}s gap2={g2:.3f}s "
                f"ratio={g2 / g1:.2f}")
        if PROFILE == "drop-first-data" and data_seen == 1:
            log(f"FAULT drop-first-data: guest data segment #1 "
                f"(seq={seq} len={len(payload)}) dropped "
                f"(no ACK, no echo)")
            return
        if PROFILE == "drop-first-ack" and data_seen == 1:
            stored = payload
            log(f"FAULT drop-first-ack: ACK for guest data segment "
                f"#1 (seq={seq}) withheld (emulating a dropped ACK); "
                f"data stored for a single echo")
            return
        if PROFILE == "delay-echo" and data_seen == 1:
            stored = payload
            echo_due = time.time() + DELAY_S
            log(f"FAULT delay-echo: echo of seq={seq} delayed by "
                f"{DELAY_S}s (past the ~1s retransmit threshold)")
            return
        if PROFILE == "delay-echo" and echo_due is not None \
                and not echo_sent:
            # spurious retransmission: dedupe with a pure ACK
            send_tcp(nxt, seq + len(payload), F_ACK,
                     note=" (dedupe ACK for retransmitted data)")
            return
        if stored is None:
            stored = payload
        if PROFILE == "ooo" and ooo_stage == 0:
            echo_seq = nxt
            half = len(stored) // 2
            send_tcp(nxt, seq + len(payload), F_ACK)
            log(f"FAULT ooo: echo segment 2 (seq={echo_seq + half}) "
                f"sent before segment 1 (seq={echo_seq})")
            send_tcp(echo_seq + half, seq + len(payload),
                     F_PSH | F_ACK, stored[half:])
            send_tcp(echo_seq, seq + len(payload),
                     F_PSH | F_ACK, stored[:half])
            ooo_stage = 1
            return
        if not echo_sent and PROFILE != "ooo":
            echo_sent = True
            send_tcp(nxt, seq + len(payload), F_PSH | F_ACK, stored)
            state = start_fin(seq + len(payload))
        return
    if PROFILE == "ooo" and ooo_stage in (1, 2) and (flags & F_ACK):
        echo_seq = ISS + 1 + len(GREETING)
        if ack == echo_seq and ooo_stage == 1:
            log(f"GUEST DUP-ACK: guest ack stayed at {ack} after the "
                f"out-of-order segment (dropped, not buffered)")
            return
        if ack == echo_seq + len(stored) // 2 and ooo_stage == 1:
            log(f"GUEST HALF-ACK: guest ack={ack} covers only "
                f"the in-order half; the early segment was dropped")
            send_tcp(echo_seq + len(stored) // 2, data_seq + len(stored),
                     F_PSH | F_ACK, stored[len(stored) // 2:],
                     note=" (RESEND of the dropped segment, in order)")
            ooo_stage = 2
            return
        if ack == echo_seq + len(stored):
            state = start_fin(data_seq + len(stored))
            ooo_stage = 3
            return
    if flags & F_FIN:
        guest_fin = True
        send_tcp(nxt, seq + 1, F_ACK)
    if fin_seq is not None and (flags & F_ACK) and ack == fin_seq + 1:
        acked_fin = True
    if guest_fin and acked_fin:
        print("PEER OK", flush=True)
        sys.exit(0)


while time.time() < DEADLINE:
    if PROFILE == "delay-echo" and echo_due is not None \
            and not echo_sent and time.time() >= echo_due:
        echo_sent = True
        log(f"delay-echo: {DELAY_S}s elapsed, sending the echo now")
        send_tcp(nxt, data_seq + len(stored), F_PSH | F_ACK, stored)
        state = start_fin(data_seq + len(stored))
    for fr in poll_frames():
        handle(fr)
    time.sleep(0.02)
fail(f"timeout: profile={PROFILE} state={state} data_seen={data_seen} "
     f"echo_sent={echo_sent} ooo_stage={ooo_stage}")
