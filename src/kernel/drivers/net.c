/* net.c — minimal ARP + IPv4 + ICMP + UDP stack (Phase 7 inc.2/3).
 *
 * Sits on the e1000 driver's raw frame send/recv. The RX task polls
 * the ring and dispatches here:
 *  - ARP requests for our address get a reply; every ARP packet and
 *    every IPv4 packet's sender teaches the ARP cache.
 *  - ICMP echo requests get echo replies; echo replies are matched
 *    against the one outstanding ping (net_ping_send) for RTT.
 *  - UDP datagrams to port 7 are echoed back (payload unchanged).
 *  - Anything else is logged to serial like the inc.1 raw build did.
 * One outstanding ping at a time; the shell is the only client.
 */
#include "net.h"
#include "e1000.h"
#include "../arch/port_io.h"
#include "../arch/timer.h"
#include "../proc/sched.h"

static const uint8_t our_ip[4] = {192, 168, 77, 2};

static uint32_t stat_arp_rx = 0, stat_arp_tx = 0;
static uint32_t stat_icmp_rx = 0, stat_icmp_tx = 0;
static uint32_t stat_udp_rx = 0, stat_udp_tx = 0;
static uint16_t ip_id_counter = 1;

/* Outstanding ping state (written by the pinging task, completed by
 * the RX task when the echo reply lands). */
static volatile int      ping_pending = 0;
static volatile int      ping_received = 0;
static uint16_t          ping_seq = 0;
static uint32_t          ping_sent_tick = 0;
static volatile uint32_t ping_recv_tick = 0;
#define PING_ID 0x4D59   /* "MY" */

/* ---- byte order helpers ---- */
static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static int ip_eq(const uint8_t *a, const uint8_t *b)
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}
static void ip_copy(uint8_t *d, const uint8_t *s)
{
    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
}
static void mac_copy(uint8_t *d, const uint8_t *s)
{
    for (int i = 0; i < 6; i++) d[i] = s[i];
}

static uint16_t cksum(const uint8_t *p, uint32_t n)
{
    uint32_t sum = 0;
    while (n >= 2) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        n -= 2;
    }
    if (n)
        sum += ((uint32_t)p[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ---- tiny serial logger (COM1), same idiom as e1000.c ---- */
static void nputc(char c)
{
    while (!(inb(0x3FD) & 0x20)) { }
    outb(0x3F8, (uint8_t)c);
}
static void nputs(const char *s) { while (*s) nputc(*s++); }
static void nputu(uint32_t v)
{
    char b[12]; int i = 0;
    if (!v) { nputc('0'); return; }
    while (v) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) nputc(b[--i]);
}
static void nputhex(uint32_t v, int digits)
{
    static const char h[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--)
        nputc(h[(v >> (i * 4)) & 0xF]);
}
static void nputip(const uint8_t *ip)
{
    for (int i = 0; i < 4; i++) {
        if (i) nputc('.');
        nputu(ip[i]);
    }
}

static void log_frame(const uint8_t *f, uint16_t len)
{
    nputs("[net] rx len=");
    nputu(len);
    if (len >= 14) {
        nputs(" type=0x");
        nputhex(be16(f + 12), 4);
        nputs(" data=\"");
        uint16_t n = len - 14;
        if (n > 48) n = 48;
        for (uint16_t i = 0; i < n; i++) {
            uint8_t c = f[14 + i];
            nputc((c >= 32 && c < 127) ? (char)c : '.');
        }
        nputc('"');
    }
    nputs("\r\n");
}

/* ---- ARP cache (8 entries, round-robin replacement) ---- */
#define ARP_CACHE_N 8
static struct { uint8_t ip[4]; uint8_t mac[6]; uint8_t valid; } arp_cache[ARP_CACHE_N];
static uint32_t arp_cache_next = 0;

static void arp_learn(const uint8_t *ip, const uint8_t *mac)
{
    for (int i = 0; i < ARP_CACHE_N; i++) {
        if (arp_cache[i].valid && ip_eq(arp_cache[i].ip, ip)) {
            mac_copy(arp_cache[i].mac, mac);
            return;
        }
    }
    uint32_t slot = arp_cache_next;
    arp_cache_next = (arp_cache_next + 1) % ARP_CACHE_N;
    ip_copy(arp_cache[slot].ip, ip);
    mac_copy(arp_cache[slot].mac, mac);
    arp_cache[slot].valid = 1;
}

int net_arp_lookup(const uint8_t ip[4], uint8_t mac_out[6])
{
    for (int i = 0; i < ARP_CACHE_N; i++) {
        if (arp_cache[i].valid && ip_eq(arp_cache[i].ip, ip)) {
            mac_copy(mac_out, arp_cache[i].mac);
            return 1;
        }
    }
    return 0;
}

/* Build + send an ARP packet (op 1 = request broadcast, 2 = reply). */
static void arp_send(uint16_t op, const uint8_t *dst_mac,
                     const uint8_t *target_mac, const uint8_t *target_ip)
{
    uint8_t f[60];
    uint8_t our_mac[6];
    e1000_mac_addr(our_mac);
    for (int i = 0; i < 60; i++) f[i] = 0;
    mac_copy(f, dst_mac);
    mac_copy(f + 6, our_mac);
    put_be16(f + 12, 0x0806);
    uint8_t *a = f + 14;
    put_be16(a + 0, 1);          /* Ethernet */
    put_be16(a + 2, 0x0800);     /* IPv4 */
    a[4] = 6;
    a[5] = 4;
    put_be16(a + 6, op);
    mac_copy(a + 8, our_mac);
    ip_copy(a + 14, our_ip);
    mac_copy(a + 18, target_mac);
    ip_copy(a + 24, target_ip);
    if (e1000_send(f, 60) == 0)
        stat_arp_tx++;
}

void net_arp_request(const uint8_t ip[4])
{
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    arp_send(1, bcast, zero, ip);
}

static void arp_input(const uint8_t *f, uint16_t len)
{
    if (len < 14 + 28)
        return;
    const uint8_t *a = f + 14;
    if (be16(a + 0) != 1 || be16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4)
        return;
    stat_arp_rx++;
    uint16_t op = be16(a + 6);
    const uint8_t *sha = a + 8, *spa = a + 14, *tpa = a + 24;
    arp_learn(spa, sha);
    if (op == 1 && ip_eq(tpa, our_ip)) {
        arp_send(2, sha, sha, spa);
        nputs("[net] arp reply to ");
        nputip(spa);
        nputs("\r\n");
    }
}

/* ---- IPv4 send path (shared by ICMP + UDP) ---- */
static int send_ipv4(const uint8_t *dst_mac, const uint8_t *dst_ip,
                     uint8_t proto, const uint8_t *payload, uint16_t plen)
{
    static uint8_t f[2048];
    if (14 + 20 + plen > (int)sizeof(f))
        return -1;
    uint8_t our_mac[6];
    e1000_mac_addr(our_mac);
    mac_copy(f, dst_mac);
    mac_copy(f + 6, our_mac);
    put_be16(f + 12, 0x0800);
    uint8_t *ip = f + 14;
    ip[0] = 0x45;
    ip[1] = 0;
    put_be16(ip + 2, (uint16_t)(20 + plen));
    put_be16(ip + 4, ip_id_counter++);
    put_be16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = proto;
    ip[10] = 0; ip[11] = 0;
    ip_copy(ip + 12, our_ip);
    ip_copy(ip + 16, dst_ip);
    put_be16(ip + 10, cksum(ip, 20));
    for (uint16_t i = 0; i < plen; i++)
        ip[20 + i] = payload[i];
    uint16_t frame_len = (uint16_t)(14 + 20 + plen);
    if (frame_len < 60)
        frame_len = 60;
    return e1000_send(f, frame_len);
}

/* ---- ICMP ---- */
static void icmp_input(const uint8_t *f, uint16_t len, uint16_t ip_total)
{
    const uint8_t *ip = f + 14;
    uint32_t ihl = (uint32_t)(ip[0] & 0x0F) * 4;
    const uint8_t *icmp = ip + ihl;
    uint32_t icmp_len = (uint32_t)ip_total - ihl;
    if (len < 14 + ihl + 8 || icmp_len < 8)
        return;

    if (icmp[0] == 0) {          /* echo reply: match the pending ping */
        if (ping_pending && be16(icmp + 4) == PING_ID
                          && be16(icmp + 6) == ping_seq) {
            ping_recv_tick = timer_ticks();
            ping_received = 1;
            ping_pending = 0;
        }
        return;
    }
    if (icmp[0] != 8)            /* echo request only */
        return;
    stat_icmp_rx++;

    static uint8_t seg[2048 - 14 - 20];
    if (icmp_len > sizeof(seg))
        return;
    for (uint32_t i = 0; i < icmp_len; i++)
        seg[i] = icmp[i];
    seg[0] = 0;                  /* echo reply */
    seg[2] = 0; seg[3] = 0;
    put_be16(seg + 2, cksum(seg, icmp_len));
    if (send_ipv4(f + 6, ip + 12, 1, seg, (uint16_t)icmp_len) == 0) {
        stat_icmp_tx++;
        nputs("[net] icmp echo reply to ");
        nputip(ip + 12);
        nputs("\r\n");
    }
}

int net_ping_send(const uint8_t ip[4], const uint8_t mac[6], uint16_t seq)
{
    static const char payload[] = "myos-ping-payload";
    uint8_t seg[8 + sizeof(payload) - 1];
    seg[0] = 8;                  /* echo request */
    seg[1] = 0;
    seg[2] = 0; seg[3] = 0;
    put_be16(seg + 4, PING_ID);
    put_be16(seg + 6, seq);
    for (uint32_t i = 0; i < sizeof(payload) - 1; i++)
        seg[8 + i] = (uint8_t)payload[i];
    put_be16(seg + 2, cksum(seg, sizeof(seg)));
    ping_seq = seq;
    ping_received = 0;
    ping_pending = 1;
    ping_sent_tick = timer_ticks();
    if (send_ipv4(mac, ip, 1, seg, (uint16_t)sizeof(seg)) != 0) {
        ping_pending = 0;
        return -1;
    }
    stat_icmp_tx++;
    return 0;
}

int net_ping_result(uint32_t *rtt_ticks)
{
    if (ping_received) {
        *rtt_ticks = ping_recv_tick - ping_sent_tick;
        ping_received = 0;
        return 1;
    }
    if (ping_pending)
        return 0;
    return -1;
}

/* ---- UDP: echo server on port 7 ---- */
static void udp_input(const uint8_t *f, uint16_t len, uint16_t ip_total)
{
    const uint8_t *ip = f + 14;
    uint32_t ihl = (uint32_t)(ip[0] & 0x0F) * 4;
    const uint8_t *udp = ip + ihl;
    uint32_t avail = (uint32_t)ip_total - ihl;
    if (len < 14 + ihl + 8 || avail < 8)
        return;
    uint16_t ulen = be16(udp + 4);
    if (ulen < 8 || ulen > avail)
        return;
    if (be16(udp + 2) != 7)      /* echo port only */
        return;
    stat_udp_rx++;

    static uint8_t seg[2048 - 14 - 20];
    if (ulen > sizeof(seg))
        return;
    for (uint32_t i = 0; i < ulen; i++)
        seg[i] = udp[i];
    put_be16(seg + 0, be16(udp + 2));   /* src = the echo port */
    put_be16(seg + 2, be16(udp + 0));   /* dst = sender's port */
    seg[6] = 0; seg[7] = 0;             /* checksum 0 (legal in IPv4) */
    if (send_ipv4(f + 6, ip + 12, 17, seg, ulen) == 0) {
        stat_udp_tx++;
        nputs("[net] udp echo reply to ");
        nputip(ip + 12);
        nputs("\r\n");
    }
}

static void ipv4_input(const uint8_t *f, uint16_t len)
{
    if (len < 14 + 20)
        return;
    const uint8_t *ip = f + 14;
    if ((ip[0] >> 4) != 4)
        return;
    uint16_t total = be16(ip + 2);
    if (total < 20 || 14 + total > len)
        return;
    static const uint8_t bcast_ip[4] = {255, 255, 255, 255};
    if (!ip_eq(ip + 16, our_ip) && !ip_eq(ip + 16, bcast_ip))
        return;
    arp_learn(ip + 12, f + 6);
    if (ip[9] == 1)
        icmp_input(f, len, total);
    else if (ip[9] == 17)
        udp_input(f, len, total);
}

void net_input(const uint8_t *f, uint16_t len)
{
    if (len < 14)
        return;
    uint16_t type = be16(f + 12);
    if (type == 0x0806)
        arp_input(f, len);
    else if (type == 0x0800)
        ipv4_input(f, len);
    else
        log_frame(f, len);
}

void net_rx_task(void)
{
    uint8_t frame[2048];
    uint16_t len;
    for (;;) {
        int got = 0;
        while (e1000_recv(frame, &len)) {
            got = 1;
            net_input(frame, len);
        }
        if (!got)
            sched_block_current((uint64_t)timer_ticks() + 2);
    }
}

void net_stack_stats(uint32_t *arp_rx, uint32_t *arp_tx,
                     uint32_t *icmp_rx, uint32_t *icmp_tx,
                     uint32_t *udp_rx, uint32_t *udp_tx)
{
    *arp_rx = stat_arp_rx;
    *arp_tx = stat_arp_tx;
    *icmp_rx = stat_icmp_rx;
    *icmp_tx = stat_icmp_tx;
    *udp_rx = stat_udp_rx;
    *udp_tx = stat_udp_tx;
}
