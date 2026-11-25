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


if PROFILE == "reno":
    # Client role: QEMU listens, we connect (retrying while it
    # boots). PORT becomes our source port; gport the guest's.
    PORT = 40001
    conn = None
    t_end = time.time() + 100
    while time.time() < t_end:
        try:
            conn = socket.create_connection(("127.0.0.1", 10000),
                                            timeout=5)
            break
        except OSError:
            time.sleep(1)
    if conn is None:
        fail("reno: could not connect to the QEMU listener")
else:
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


SEND_WIN = 2048


def send_tcp(seq, ack, flags, payload=b"", note="", win=None):
    global nxt
    seg = struct.pack(">HHII", PORT, gport, seq, ack) \
        + struct.pack(">BBHHH", 5 << 4, flags,
                      SEND_WIN if win is None else win, 0, 0) + payload
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
    global echo_due, echo_sent, ooo_stage, guest_fin, acked_fin, nxt
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
                f"out-of-order segment (gap signalled; under "
                f"reassembly the segment is buffered, not dropped)")
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
            # Segment 1 was sent last, so nxt undercounts the
            # stream end; the FIN must go out at the true end.
            nxt = echo_seq + len(stored)
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


# ------------------------------------------------------------------
# reno profile (Phase 15 inc.5): the peer is the TCP CLIENT and
# the guest runs tcpechosrv, so the guest is the sender under
# test. The peer drives the guest's cwnd up in slow start with
# three clean echo rounds, then manufactures duplicate ACKs for
# the fourth echo segment (with a window change in the middle
# of the run, which must reset the count), and finally withholds
# an ACK entirely to force two timer retransmits of the sixth.
RENO_ISS = 3000
rsegs = []          # (t, seq, ack, flags, payload) guest segments
rsynacks = [0]      # SYN-ACKs seen after the handshake grace window
rcount_syn = [False]


def rparse(fr):
    if len(fr) < 54 or fr[12:14] != b"\x08\x00":
        return None
    ip = fr[14:]
    if ip[9] != 6 or ip[12:16] != GUEST_IP:
        return None
    ihl = (ip[0] & 0x0F) * 4
    t = ip[ihl:]
    total = struct.unpack(">H", ip[2:4])[0]
    ln = total - ihl - (t[12] >> 4) * 4
    sp, dp = struct.unpack(">HH", t[:4])
    if sp != 8080 or dp != 40001:
        return None
    rseg = t[:total - ihl]
    if cksum(GUEST_IP + PEER_IP + struct.pack(">BBH", 0, 6, len(rseg)) + rseg) != 0:
        fail("reno: guest segment with a bad checksum")
    seq, ack = struct.unpack(">II", t[4:12])
    return (time.time(), seq, ack, t[13], t[(t[12] >> 4) * 4:(t[12] >> 4) * 4 + ln])


def rpump(dur):
    end = time.time() + dur
    while time.time() < end:
        for fr in poll_frames():
            s = rparse(fr)
            if s is not None:
                rsegs.append(s)
                log(f"<- seq={s[1]} ack={s[2]} {fname(s[3])} len={len(s[4])}")
                if rcount_syn[0] and (s[3] & (F_SYN | F_ACK)) == (F_SYN | F_ACK):
                    rsynacks[0] += 1
        time.sleep(0.005)


rseen = [0]         # persistent cursor: segments are consumed in
                    # arrival order by exactly one wait


def rwait(pred, what, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        rpump(0.05)
        while rseen[0] < len(rsegs):
            s = rsegs[rseen[0]]
            rseen[0] += 1
            if pred(s):
                return s
    fail(f"reno: timeout waiting for {what}")


def rcount(seq):
    return sum(1 for s in rsegs if s[1] == seq and len(s[4]) > 0)


def reno_main():
    global gport, SEND_WIN
    gport = 8080
    SEND_WIN = 4096
    # Handshake, retrying the SYN until the guest is listening.
    gseq0 = None
    end = time.time() + 90
    while time.time() < end and gseq0 is None:
        send_tcp(RENO_ISS, 0, F_SYN)
        # Pump in short slices and ACK the SYN-ACK the moment it
        # is seen: a slow final ACK lets the guest's SYN-ACK RTO
        # (1s initial) fire, and a retransmitted SYN-ACK rightly
        # suppresses the guest's handshake cwnd growth.
        slice_end = time.time() + 1.5
        while time.time() < slice_end and gseq0 is None:
            rpump(0.05)
            for s in rsegs:
                if (s[3] & (F_SYN | F_ACK)) == (F_SYN | F_ACK):
                    gseq0 = s[1]
                    break
        rsegs.clear()
    if gseq0 is None:
        fail("reno: no SYN-ACK from the guest")
    myseq = RENO_ISS + 1
    gnext = gseq0 + 1
    # Delay the final ACK by 400ms (well inside the guest's 1s
    # initial SYN-ACK RTO): the guest's handshake RTT sample is
    # then ~40 ticks, so its RTO for the first data segment is
    # ~400ms rather than the 200ms floor, and the clean rounds
    # below (350ms ACK delays) stay inside one RTO throughout.
    time.sleep(0.4)
    send_tcp(myseq, gnext, F_ACK)
    # Grace window: SYN-ACKs answering our own queued retry SYNs
    # may still straggle in and are ignored. After it, any further
    # SYN-ACK would be a guest retransmit (which would also have
    # suppressed the guest's handshake cwnd growth) and fails the
    # final check.
    rpump(0.5)
    rsegs.clear()
    rcount_syn[0] = True
    log("reno: handshake complete")

    def roundtrip(data, ackit=True, ack_delay=0.0):
        nonlocal myseq, gnext
        send_tcp(myseq, gnext, F_PSH | F_ACK, data)
        myseq += len(data)
        s = rwait(lambda x: len(x[4]) > 0 and x[1] == gnext,
                  f"echo of {data!r}")
        if s[4] != data:
            fail(f"reno: echo mismatch: {s[4]!r} != {data!r}")
        gnext += len(data)
        if ackit:
            # ack_delay on the clean rounds gives the guest real
            # RTT samples (~35 ticks), lifting its RTO to ~350ms
            # so the duplicate-ACK phase below runs with margin
            # inside one RTO instead of racing the 200ms floor.
            if ack_delay:
                time.sleep(ack_delay)
            send_tcp(myseq, gnext, F_ACK)
        return s

    for d in (b"reno-0001", b"reno-0002", b"reno-0003"):
        roundtrip(d, ack_delay=0.35)
    log("reno: three clean rounds done (guest cwnd should be 5120)")

    # Round 4: duplicate ACKs, with a window change mid-run.
    s4 = roundtrip(b"reno-0004", ackit=False)
    S4 = s4[1]
    for _ in range(2):
        send_tcp(myseq, S4, F_ACK)
        time.sleep(0.01)
    SEND_WIN = 2048     # window update: must reset the dup count
    send_tcp(myseq, S4, F_ACK)
    time.sleep(0.01)
    for _ in range(2):
        send_tcp(myseq, S4, F_ACK)
        time.sleep(0.01)
    rpump(0.05)
    if rcount(S4) != 1:
        fail(f"reno: fast retransmit fired early (seg4 count "
             f"{rcount(S4)} after 2+2 dups around a window change)")
    t_dup3 = time.time()
    send_tcp(myseq, S4, F_ACK)      # third consecutive dup
    sfr = rwait(lambda x: len(x[4]) > 0 and x[1] == S4,
                "fast retransmit of seg4", timeout=5)
    dt = sfr[0] - t_dup3
    log(f"RENO fast-retransmit dt={dt:.3f}")
    if dt >= 0.060:
        fail(f"reno: fast retransmit too slow (dt={dt:.3f}s); "
             f"cannot be distinguished from the 200ms RTO path")
    for _ in range(3):              # more dups: still one resend
        send_tcp(myseq, S4, F_ACK)
        time.sleep(0.01)
    rpump(0.05)
    if rcount(S4) != 2:
        fail(f"reno: seg4 sent {rcount(S4)} times, expected 2 "
             f"(in-recovery suppression broken)")
    gnext = S4 + len(b"reno-0004")
    send_tcp(myseq, gnext, F_ACK)   # recovery ACK
    log("reno: fast retransmit verified, recovery ACK sent")

    roundtrip(b"reno-0005", ack_delay=0.35)   # congestion-avoidance round
    # Round 6: withhold the ACK through two timer retransmits.
    s6 = roundtrip(b"reno-0006", ackit=False)
    S6 = s6[1]
    r1 = rwait(lambda x: len(x[4]) > 0 and x[1] == S6,
               "first timer retransmit of seg6", timeout=10)
    r2 = rwait(lambda x: len(x[4]) > 0 and x[1] == S6,
               "second timer retransmit of seg6", timeout=10)
    g1 = r1[0] - s6[0]
    g2 = r2[0] - r1[0]
    log(f"RENO timeout gaps g1={g1:.3f} g2={g2:.3f}")
    if g2 < 0.30:
        fail(f"reno: second retransmit gap {g2:.3f}s < 0.30s; "
             f"RTO backoff not applied")
    gnext = S6 + len(b"reno-0006")
    send_tcp(myseq, gnext, F_ACK)
    # Close: our FIN, then the guest's FIN, then ACK it.
    send_tcp(myseq, gnext, F_FIN | F_ACK)
    myseq += 1
    rwait(lambda x: x[3] & F_FIN, "guest FIN", timeout=15)
    send_tcp(myseq, gnext + 1, F_ACK)
    rpump(0.3)
    if rsynacks[0] != 0:
        fail(f"reno: guest retransmitted its SYN-ACK "
             f"({rsynacks[0]} extra) after the handshake")
    log("reno: close complete")
    print("PEER OK", flush=True)
    sys.exit(0)


if PROFILE == "reno":
    reno_main()

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
