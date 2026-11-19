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

static uint8_t our_ip[4]   = {192, 168, 77, 2};   /* static default; DHCP can rebind */
static uint8_t our_mask[4] = {255, 255, 255, 0};
static uint8_t our_gw[4]   = {0, 0, 0, 0};
static uint8_t our_dns[4]  = {0, 0, 0, 0};

static uint32_t stat_arp_rx = 0, stat_arp_tx = 0;
static uint32_t stat_icmp_rx = 0, stat_icmp_tx = 0;
static uint32_t stat_udp_rx = 0, stat_udp_tx = 0;
static uint32_t stat_bad_cksum = 0;   /* Phase 11: dropped on checksum */
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
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | p[3];
}
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
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

static uint16_t l4_cksum(const uint8_t *src, const uint8_t *dst,
                         uint8_t proto, const uint8_t *seg, uint32_t len);

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
static int send_ipv4_from(const uint8_t *src_ip, const uint8_t *dst_mac,
                          const uint8_t *dst_ip, uint8_t proto,
                          const uint8_t *payload, uint16_t plen)
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
    ip_copy(ip + 12, src_ip);
    ip_copy(ip + 16, dst_ip);
    put_be16(ip + 10, cksum(ip, 20));
    for (uint16_t i = 0; i < plen; i++)
        ip[20 + i] = payload[i];
    uint16_t frame_len = (uint16_t)(14 + 20 + plen);
    if (frame_len < 60)
        frame_len = 60;
    return e1000_send(f, frame_len);
}

static int send_ipv4(const uint8_t *dst_mac, const uint8_t *dst_ip,
                     uint8_t proto, const uint8_t *payload, uint16_t plen)
{
    return send_ipv4_from(our_ip, dst_mac, dst_ip, proto, payload, plen);
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
    if (cksum(icmp, icmp_len) != 0) {   /* Phase 11: verify */
        stat_bad_cksum++;
        return;
    }

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


/* ---- DHCP client (Phase 8 inc.3) ----
 * Driven by SYS_DHCP: net_dhcp_start() broadcasts a DISCOVER (from
 * 0.0.0.0, broadcast-reply flag set so answers come to the broadcast
 * address our IPv4 input accepts before we have an address), the RX
 * task's dhcp_input() records the OFFER, net_dhcp_request() asks for
 * it, and the ACK commits our_ip/mask/gw/dns. */
static volatile int dhcp_state = 0;   /* 0 idle, 1 discover, 2 request, 3 bound, -1 nak */
static volatile int dhcp_have_offer = 0;
static uint32_t dhcp_xid = 0;
static uint8_t dhcp_offer_ip[4];
static uint8_t dhcp_server_ip[4];
static uint8_t dhcp_opt_mask[4];
static uint8_t dhcp_opt_gw[4];
static uint8_t dhcp_opt_dns[4];

static void dhcp_send(uint8_t msg_type)
{
    static uint8_t bootp[300];
    for (int i = 0; i < 300; i++) bootp[i] = 0;
    bootp[0] = 1; bootp[1] = 1; bootp[2] = 6; bootp[3] = 0;
    put_be32(bootp + 4, dhcp_xid);
    put_be16(bootp + 10, 0x8000);          /* want broadcast replies */
    uint8_t mac[6];
    e1000_mac_addr(mac);
    mac_copy(bootp + 28, mac);
    bootp[236] = 0x63; bootp[237] = 0x82; bootp[238] = 0x53; bootp[239] = 0x63;
    uint8_t *o = bootp + 240;
    *o++ = 53; *o++ = 1; *o++ = msg_type;
    if (msg_type == 3) {                   /* REQUEST: what we want */
        *o++ = 50; *o++ = 4; ip_copy(o, dhcp_offer_ip); o += 4;
        *o++ = 54; *o++ = 4; ip_copy(o, dhcp_server_ip); o += 4;
    }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;
    *o++ = 255;
    uint16_t blen = (uint16_t)(o - bootp);
    static uint8_t udp[8 + 300];
    put_be16(udp + 0, 68);
    put_be16(udp + 2, 67);
    put_be16(udp + 4, (uint16_t)(8 + blen));
    udp[6] = 0; udp[7] = 0;
    for (uint16_t i = 0; i < blen; i++) udp[8 + i] = bootp[i];
    static const uint8_t zero_ip[4] = {0, 0, 0, 0};
    static const uint8_t bip[4] = {255, 255, 255, 255};
    static const uint8_t bmac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    send_ipv4_from(zero_ip, bmac, bip, 17, udp, (uint16_t)(8 + blen));
}

void net_dhcp_start(void)
{
    dhcp_xid = (uint32_t)timer_ticks() ^ 0x4D590000u;
    dhcp_have_offer = 0;
    dhcp_state = 1;
    dhcp_send(1);                          /* DISCOVER */
}

void net_dhcp_request(void)
{
    dhcp_state = 2;
    dhcp_send(3);                          /* REQUEST */
}

int net_dhcp_state(void) { return dhcp_state; }
int net_dhcp_have_offer(void) { return dhcp_have_offer; }

static void dhcp_input(const uint8_t *udp, uint32_t ulen)
{
    const uint8_t *p = udp + 8;
    uint32_t plen = ulen - 8;
    if (plen < 240 || p[0] != 2)
        return;
    if (be32(p + 4) != dhcp_xid)
        return;
    if (p[236] != 0x63 || p[237] != 0x82 || p[238] != 0x53 || p[239] != 0x63)
        return;
    uint8_t msg_type = 0;
    uint8_t server[4] = {0, 0, 0, 0};
    int have_server = 0;
    uint32_t o = 240;
    while (o + 1 < plen) {
        uint8_t code = p[o];
        if (code == 255)
            break;
        if (code == 0) { o++; continue; }
        uint8_t ln = p[o + 1];
        if (o + 2 + ln > plen)
            break;
        const uint8_t *v = p + o + 2;
        if (code == 53 && ln >= 1)
            msg_type = v[0];
        else if (code == 54 && ln >= 4) { ip_copy(server, v); have_server = 1; }
        else if (code == 1 && ln >= 4)
            ip_copy(dhcp_opt_mask, v);
        else if (code == 3 && ln >= 4)
            ip_copy(dhcp_opt_gw, v);
        else if (code == 6 && ln >= 4)
            ip_copy(dhcp_opt_dns, v);
        o += 2 + ln;
    }
    if (msg_type == 2 && dhcp_state == 1) {          /* OFFER */
        ip_copy(dhcp_offer_ip, p + 16);
        if (have_server)
            ip_copy(dhcp_server_ip, server);
        else
            ip_copy(dhcp_server_ip, p + 20);         /* siaddr */
        dhcp_have_offer = 1;
    } else if (msg_type == 5 && dhcp_state == 2) {   /* ACK: commit */
        ip_copy(our_ip, p + 16);
        ip_copy(our_mask, dhcp_opt_mask);
        ip_copy(our_gw, dhcp_opt_gw);
        ip_copy(our_dns, dhcp_opt_dns);
        dhcp_state = 3;
        nputs("[net] dhcp bound ");
        nputip(our_ip);
        nputs(" gw ");
        nputip(our_gw);
        nputs("\r\n");
    } else if (msg_type == 6) {                      /* NAK */
        dhcp_state = -1;
    }
}

void net_our_ip(uint8_t out[4]) { ip_copy(out, our_ip); }
void net_gateway(uint8_t out[4]) { ip_copy(out, our_gw); }

void net_dhcp_info(uint8_t ip[4], uint8_t gw[4], uint8_t mask[4],
                   uint8_t dns[4])
{
    ip_copy(ip, our_ip);
    ip_copy(gw, our_gw);
    ip_copy(mask, our_mask);
    ip_copy(dns, our_dns);
}

/* Next hop for a destination: the gateway when off-link and one is
 * configured, else the destination itself. */
void net_route_ip(const uint8_t ip[4], uint8_t next_hop[4])
{
    int on_link = 1;
    for (int i = 0; i < 4; i++)
        if ((ip[i] & our_mask[i]) != (our_ip[i] & our_mask[i]))
            on_link = 0;
    static const uint8_t zero4[4] = {0, 0, 0, 0};
    if (!on_link && !ip_eq(our_gw, zero4))
        ip_copy(next_hop, our_gw);
    else
        ip_copy(next_hop, ip);
}


/* ---- DNS resolver (Phase 9 inc.1) ----
 * One outstanding A query. net_dns_query() sends it (the caller in
 * syscall.c resolves ARP first); the RX task's dns_input() matches
 * the response by id and walks the answers (names may be compressed)
 * to the first A record. */
#define DNS_CLIENT_PORT 53317
static volatile int dns_state = 0;   /* 0 idle, 1 waiting, 2 done, -1 error */
static uint16_t dns_id = 0;
static uint8_t dns_result_ip[4];

int net_dns_begin(const char *name, const uint8_t server_ip[4],
                  const uint8_t server_mac[6])
{
    static uint8_t q[320];
    uint16_t id = (uint16_t)(timer_ticks() ^ 0xD355u);
    q[0] = (uint8_t)(id >> 8); q[1] = (uint8_t)id;
    q[2] = 0x01; q[3] = 0x00;              /* RD set */
    q[4] = 0; q[5] = 1;                    /* QDCOUNT = 1 */
    q[6] = 0; q[7] = 0; q[8] = 0; q[9] = 0; q[10] = 0; q[11] = 0;
    uint32_t o = 12;
    const char *c = name;
    while (*c) {
        const char *dot = c;
        while (*dot && *dot != '.') dot++;
        uint32_t ln = (uint32_t)(dot - c);
        if (ln == 0 || ln > 63 || o + ln + 6 > sizeof(q))
            return -1;
        q[o++] = (uint8_t)ln;
        for (uint32_t i = 0; i < ln; i++) q[o++] = (uint8_t)c[i];
        c = (*dot == '.') ? dot + 1 : dot;
    }
    q[o++] = 0;
    q[o++] = 0; q[o++] = 1;                /* QTYPE A */
    q[o++] = 0; q[o++] = 1;                /* QCLASS IN */
    uint16_t qlen = (uint16_t)o;
    static uint8_t udp[8 + 320];
    put_be16(udp + 0, DNS_CLIENT_PORT);
    put_be16(udp + 2, 53);
    put_be16(udp + 4, (uint16_t)(8 + qlen));
    udp[6] = 0; udp[7] = 0;
    for (uint16_t i = 0; i < qlen; i++) udp[8 + i] = q[i];
    dns_id = id;
    dns_state = 1;
    return send_ipv4(server_mac, server_ip, 17, udp, (uint16_t)(8 + qlen));
}

int net_dns_poll(uint8_t out_ip[4])
{
    if (dns_state == 2) {
        ip_copy(out_ip, dns_result_ip);
        return 1;
    }
    return dns_state < 0 ? -1 : 0;
}

static int dns_skip_name(const uint8_t *p, uint32_t len, uint32_t off,
                         uint32_t *next)
{
    uint32_t i = off;
    for (;;) {
        if (i >= len) return -1;
        uint8_t c = p[i];
        if (c == 0) { *next = i + 1; return 0; }
        if ((c & 0xC0) == 0xC0) { *next = i + 2; return 0; }
        if (c & 0xC0) return -1;
        i += 1 + c;
    }
}

static void dns_input(const uint8_t *udp, uint32_t ulen)
{
    if (dns_state != 1 || ulen < 8 + 12)
        return;
    const uint8_t *p = udp + 8;
    uint32_t len = ulen - 8;
    if (be16(p) != dns_id)
        return;
    if ((p[3] & 0x0F) != 0) { dns_state = -1; return; }   /* rcode */
    uint16_t qd = be16(p + 4), an = be16(p + 6);
    uint32_t o = 12;
    for (uint16_t i = 0; i < qd; i++) {
        if (dns_skip_name(p, len, o, &o) < 0 || o + 4 > len) {
            dns_state = -1;
            return;
        }
        o += 4;
    }
    for (uint16_t i = 0; i < an; i++) {
        if (dns_skip_name(p, len, o, &o) < 0 || o + 10 > len) {
            dns_state = -1;
            return;
        }
        uint16_t type = be16(p + o);
        uint16_t rdlen = be16(p + o + 8);
        o += 10;
        if (o + rdlen > len) { dns_state = -1; return; }
        if (type == 1 && rdlen == 4) {
            ip_copy(dns_result_ip, p + o);
            dns_state = 2;
            return;
        }
        o += rdlen;
    }
    dns_state = -1;   /* no A record in the answers */
}

void net_dns_server(uint8_t out[4]) { ip_copy(out, our_dns); }

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
    if (be16(udp + 6) != 0      /* 0 = no checksum, legal in IPv4 */
        && l4_cksum(ip + 12, ip + 16, 17, udp, ulen) != 0) {
        stat_bad_cksum++;       /* Phase 11: verify */
        return;
    }
    if (be16(udp + 2) == 68) {   /* DHCP client port */
        dhcp_input(udp, ulen);
        return;
    }
    if (be16(udp + 2) == DNS_CLIENT_PORT) {
        dns_input(udp, ulen);
        return;
    }
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

static void tcp_input(const uint8_t *f, uint16_t len, uint16_t ip_total);
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
    uint32_t ihl = (uint32_t)(ip[0] & 0x0F) * 4;
    if (ihl < 20 || 14 + ihl > len)
        return;
    if (cksum(ip, ihl) != 0) {   /* Phase 11: verify header checksum */
        stat_bad_cksum++;
        return;
    }
    static const uint8_t bcast_ip[4] = {255, 255, 255, 255};
    if (!ip_eq(ip + 16, our_ip) && !ip_eq(ip + 16, bcast_ip))
        return;
    arp_learn(ip + 12, f + 6);
    if (ip[9] == 1)
        icmp_input(f, len, total);
    else if (ip[9] == 17)
        udp_input(f, len, total);
    else if (ip[9] == 6)
        tcp_input(f, len, total);
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


/* ---- TCP client (Phase 9 inc.2) ----
 * One connection at a time, client side only. In-order delivery
 * only (out-of-order segments get a duplicate ACK and are dropped),
 * one unacked data segment with timer-driven retransmit driven by
 * the blocking syscall's wait loop (net_tcp_retransmit). The RX
 * task feeds tcp_input(); SYS_TCPECHO orchestrates the exchange. */
#define TCP_RX_CAP 2048
#define TCP_TX_CAP 1024
#define TCP_F_FIN 0x01
#define TCP_F_SYN 0x02
#define TCP_F_RST 0x04
#define TCP_F_PSH 0x08
#define TCP_F_ACK 0x10
enum { TCP_CLOSED = 0, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT,
       TCP_CLOSE_WAIT, TCP_LAST_ACK };
static struct {
    int state;
    uint8_t peer_ip[4];
    uint8_t peer_mac[6];
    uint16_t peer_port, local_port;
    uint32_t snd_una, snd_nxt, rcv_nxt;
    uint8_t rx[TCP_RX_CAP];
    uint32_t rx_len;
    uint8_t tx[TCP_TX_CAP];
    uint32_t tx_seq, tx_len;   /* unacked data: seq + length */
    uint32_t fin_seq;
    int fin_queued, peer_fin, error;
    uint32_t last_send;
} tcp;

/* One's-complement checksum over the pseudo-header + segment.
 * For sending, call with the segment's checksum field zeroed. For
 * receive verification, call over the segment as received (checksum
 * field included): a valid segment returns 0. */
static uint16_t l4_cksum(const uint8_t *src, const uint8_t *dst,
                         uint8_t proto, const uint8_t *seg, uint32_t len)
{
    uint32_t sum = 0;
    sum += (uint16_t)((src[0] << 8) | src[1]);
    sum += (uint16_t)((src[2] << 8) | src[3]);
    sum += (uint16_t)((dst[0] << 8) | dst[1]);
    sum += (uint16_t)((dst[2] << 8) | dst[3]);
    sum += proto;
    sum += len;
    uint32_t i = 0;
    for (; i + 1 < len; i += 2)
        sum += (uint16_t)((seg[i] << 8) | seg[i + 1]);
    if (i < len)
        sum += (uint16_t)(seg[i] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t tcp_checksum(const uint8_t *src, const uint8_t *dst,
                             const uint8_t *seg, uint32_t len)
{
    return l4_cksum(src, dst, 6, seg, len);
}

static void tcp_send_raw(const uint8_t *dst_mac, const uint8_t *dst_ip,
                         uint16_t sport, uint16_t dport, uint32_t seq,
                         uint32_t ack, uint8_t flags,
                         const uint8_t *data, uint32_t dlen)
{
    static uint8_t seg[20 + TCP_TX_CAP];
    put_be16(seg + 0, sport);
    put_be16(seg + 2, dport);
    put_be32(seg + 4, seq);
    put_be32(seg + 8, ack);
    seg[12] = 5 << 4;
    seg[13] = flags;
    put_be16(seg + 14, TCP_RX_CAP);
    put_be16(seg + 16, 0);
    put_be16(seg + 18, 0);
    for (uint32_t i = 0; i < dlen; i++)
        seg[20 + i] = data[i];
    put_be16(seg + 16, tcp_checksum(our_ip, dst_ip, seg, 20 + dlen));
    send_ipv4(dst_mac, dst_ip, 6, seg, (uint16_t)(20 + dlen));
}

static void tcp_send_seg(uint8_t flags, const uint8_t *data, uint32_t dlen)
{
    tcp_send_raw(tcp.peer_mac, tcp.peer_ip, tcp.local_port, tcp.peer_port,
                 tcp.snd_nxt, tcp.rcv_nxt, flags, data, dlen);
    tcp.last_send = (uint32_t)timer_ticks();
}

void net_tcp_reset(void)
{
    tcp.state = TCP_CLOSED;
    tcp.rx_len = 0;
    tcp.tx_len = 0;
    tcp.fin_queued = 0;
    tcp.peer_fin = 0;
    tcp.error = 0;
}

int net_tcp_connect(const uint8_t ip[4], const uint8_t mac[6], uint16_t port)
{
    if (tcp.state != TCP_CLOSED)
        return -1;
    ip_copy(tcp.peer_ip, ip);
    mac_copy(tcp.peer_mac, mac);
    tcp.peer_port = port;
    tcp.local_port = (uint16_t)(49152 + (timer_ticks() & 0x3FF));
    tcp.snd_una = tcp.snd_nxt = (uint32_t)timer_ticks() ^ 0x5EED0000u;
    tcp.rcv_nxt = 0;
    tcp.rx_len = 0;
    tcp.tx_len = 0;
    tcp.fin_queued = 0;
    tcp.peer_fin = 0;
    tcp.error = 0;
    tcp.state = TCP_SYN_SENT;
    tcp_send_seg(TCP_F_SYN, 0, 0);
    tcp.snd_nxt++;   /* SYN consumes a sequence number */
    return 0;
}

int net_tcp_state(void) { return tcp.state; }
int net_tcp_error(void) { return tcp.error; }
int net_tcp_acked(void) { return tcp.tx_len == 0 && !tcp.fin_queued; }
int net_tcp_eof(void) { return tcp.peer_fin && tcp.rx_len == 0; }

int net_tcp_send(const uint8_t *data, uint32_t len)
{
    if (tcp.state != TCP_ESTABLISHED || tcp.tx_len > 0 || len > TCP_TX_CAP)
        return -1;
    for (uint32_t i = 0; i < len; i++)
        tcp.tx[i] = data[i];
    tcp.tx_seq = tcp.snd_nxt;
    tcp.tx_len = len;
    tcp_send_seg(TCP_F_PSH | TCP_F_ACK, data, len);
    tcp.snd_nxt += len;
    return 0;
}

int net_tcp_recv(uint8_t *out, uint32_t cap)
{
    uint32_t n = tcp.rx_len < cap ? tcp.rx_len : cap;
    for (uint32_t i = 0; i < n; i++)
        out[i] = tcp.rx[i];
    for (uint32_t i = n; i < tcp.rx_len; i++)
        tcp.rx[i - n] = tcp.rx[i];
    tcp.rx_len -= n;
    return (int)n;
}

void net_tcp_close(void)
{
    if (tcp.state == TCP_ESTABLISHED || tcp.state == TCP_CLOSE_WAIT) {
        tcp.fin_seq = tcp.snd_nxt;
        tcp.fin_queued = 1;
        tcp_send_seg(TCP_F_FIN | TCP_F_ACK, 0, 0);
        tcp.snd_nxt++;
        tcp.state = (tcp.state == TCP_CLOSE_WAIT) ? TCP_LAST_ACK : TCP_FIN_WAIT;
    }
}

/* Retransmit whatever is unacked if it has been out for ~1 s.
 * Called from the blocking syscall's wait loop. */
void net_tcp_retransmit(void)
{
    uint32_t now = (uint32_t)timer_ticks();
    if (now - tcp.last_send < 100)
        return;
    if (tcp.state == TCP_SYN_SENT) {
        tcp.snd_nxt--;
        tcp_send_seg(TCP_F_SYN, 0, 0);
        tcp.snd_nxt++;
    } else if (tcp.tx_len > 0 && tcp.snd_una < tcp.tx_seq + tcp.tx_len) {
        uint32_t off = tcp.snd_una - tcp.tx_seq;
        uint32_t n = tcp.tx_len - off;
        uint32_t save = tcp.snd_nxt;
        tcp.snd_nxt = tcp.snd_una;
        tcp_send_seg(TCP_F_PSH | TCP_F_ACK, tcp.tx + off, n);
        tcp.snd_nxt = save;
    } else if (tcp.fin_queued && tcp.snd_una <= tcp.fin_seq) {
        uint32_t save = tcp.snd_nxt;
        tcp.snd_nxt = tcp.fin_seq;
        tcp_send_seg(TCP_F_FIN | TCP_F_ACK, 0, 0);
        tcp.snd_nxt = save;
    }
}

static void tcp_input(const uint8_t *f, uint16_t len, uint16_t ip_total)
{
    (void)len;   /* frame length; the IP total length is authoritative */
    const uint8_t *ip = f + 14;
    uint16_t ihl = (uint16_t)((ip[0] & 0x0F) * 4);
    if (ip_total < (uint16_t)(ihl + 20))
        return;
    const uint8_t *t = ip + ihl;
    uint16_t seg_len = (uint16_t)(ip_total - ihl);
    uint16_t doff = (uint16_t)((t[12] >> 4) * 4);
    if (doff < 20 || seg_len < doff)
        return;
    const uint8_t *data = t + doff;
    uint32_t dlen = seg_len - doff;
    uint16_t sport = be16(t), dport = be16(t + 2);
    uint32_t seq = be32(t + 4), ack = be32(t + 8);
    uint8_t flags = t[13];
    const uint8_t *src_ip = ip + 12;

    if (l4_cksum(src_ip, ip + 16, 6, t, seg_len) != 0) {
        stat_bad_cksum++;   /* Phase 11: drop silently, not even an RST */
        return;
    }
    if (tcp.state == TCP_CLOSED) {
        if (flags & TCP_F_RST)
            return;
        /* RFC 793 reset for a segment with no connection. */
        if (flags & TCP_F_ACK)
            tcp_send_raw(f + 6, src_ip, dport, sport, ack, 0, TCP_F_RST, 0, 0);
        else
            tcp_send_raw(f + 6, src_ip, dport, sport, 0,
                         seq + dlen + ((flags & TCP_F_SYN) ? 1 : 0)
                             + ((flags & TCP_F_FIN) ? 1 : 0),
                         TCP_F_RST | TCP_F_ACK, 0, 0);
        return;
    }
    if (!ip_eq(src_ip, tcp.peer_ip) || sport != tcp.peer_port
        || dport != tcp.local_port)
        return;
    if (flags & TCP_F_RST) {
        tcp.error = 1;
        tcp.state = TCP_CLOSED;
        return;
    }
    if (tcp.state == TCP_SYN_SENT) {
        if ((flags & (TCP_F_SYN | TCP_F_ACK)) == (TCP_F_SYN | TCP_F_ACK)
            && ack == tcp.snd_nxt) {
            tcp.snd_una = ack;
            tcp.rcv_nxt = seq + 1;
            tcp.state = TCP_ESTABLISHED;
            tcp_send_seg(TCP_F_ACK, 0, 0);
        }
        return;
    }
    /* ACK processing: advance snd_una over data and FIN. */
    if ((flags & TCP_F_ACK) && ack > tcp.snd_una && ack <= tcp.snd_nxt) {
        tcp.snd_una = ack;
        if (tcp.tx_len > 0 && ack >= tcp.tx_seq + tcp.tx_len)
            tcp.tx_len = 0;
        if (tcp.fin_queued && ack > tcp.fin_seq) {
            tcp.fin_queued = 0;
            if (tcp.state == TCP_LAST_ACK)
                tcp.state = TCP_CLOSED;
        }
    }
    /* In-order data. */
    if (dlen > 0) {
        if (seq == tcp.rcv_nxt) {
            uint32_t room = TCP_RX_CAP - tcp.rx_len;
            uint32_t n = dlen < room ? dlen : room;
            for (uint32_t i = 0; i < n; i++)
                tcp.rx[tcp.rx_len + i] = data[i];
            tcp.rx_len += n;
            tcp.rcv_nxt += n;
        }
        tcp_send_seg(TCP_F_ACK, 0, 0);   /* ack (or dup-ack) */
    }
    /* FIN, in order after any data in this segment. */
    if ((flags & TCP_F_FIN) && seq + dlen == tcp.rcv_nxt) {
        tcp.rcv_nxt++;
        tcp.peer_fin = 1;
        tcp_send_seg(TCP_F_ACK, 0, 0);
        if (tcp.state == TCP_ESTABLISHED)
            tcp.state = TCP_CLOSE_WAIT;
        else if (tcp.state == TCP_FIN_WAIT)
            tcp.state = TCP_CLOSED;
    }
}

void net_rx_task(void)
{
    uint8_t frame[2048];
    uint16_t len;
    e1000_set_rx_task(sched_current_task());
    for (;;) {
        int got = 0;
        while (e1000_recv(frame, &len)) {
            got = 1;
            net_input(frame, len);
        }
        if (!got)
            /* IRQ wakes us on arrival; the 50-tick deadline is a
             * fallback poll in case an interrupt is ever lost. */
            sched_block_current_io((uint64_t)timer_ticks() + 50);
    }
}

void net_stack_stats(uint32_t *arp_rx, uint32_t *arp_tx,
                     uint32_t *icmp_rx, uint32_t *icmp_tx,
                     uint32_t *udp_rx, uint32_t *udp_tx,
                     uint32_t *bad_cksum)
{
    *arp_rx = stat_arp_rx;
    *arp_tx = stat_arp_tx;
    *icmp_rx = stat_icmp_rx;
    *icmp_tx = stat_icmp_tx;
    *udp_rx = stat_udp_rx;
    *udp_tx = stat_udp_tx;
    *bad_cksum = stat_bad_cksum;
}
