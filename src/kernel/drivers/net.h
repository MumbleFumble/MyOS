#pragma once
#include <stdint.h>

/* Minimal network stack (Phase 7): ARP + IPv4 + ICMP echo + UDP echo.
 * Static IPv4 address 192.168.77.2/24 (the socket-backend test peers
 * play the network; for a future slirp backend the guest address
 * would be 10.0.2.15). See net.c. */

/* Kernel task body: drains the e1000 RX ring into net_input(). */
void net_rx_task(void);

/* Dispatch one received Ethernet frame (ARP / IPv4 / log others). */
void net_input(const uint8_t *frame, uint16_t len);

/* ARP cache: 1 if ip is known (mac_out filled), else 0. */
int  net_arp_lookup(const uint8_t ip[4], uint8_t mac_out[6]);
/* Broadcast an ARP request for ip (async; the reply fills the cache). */
void net_arp_request(const uint8_t ip[4]);

/* Send an ICMP echo request to ip via mac and arm the pending state.
 * Returns 0 on send. net_ping_result() then reports the outcome:
 * 1 = reply received (*rtt_ticks filled), 0 = still waiting,
 * -1 = nothing pending. */
int  net_ping_send(const uint8_t ip[4], const uint8_t mac[6], uint16_t seq);
int  net_ping_result(uint32_t *rtt_ticks);

/* DHCP client (Phase 8 inc.3), driven by SYS_DHCP. */
void net_dhcp_start(void);
void net_dhcp_request(void);
int  net_dhcp_state(void);       /* 0 idle, 1 discover, 2 request, 3 bound, -1 nak */
int  net_dhcp_have_offer(void);
void net_dhcp_info(uint8_t ip[4], uint8_t gw[4], uint8_t mask[4],
                   uint8_t dns[4]);
void net_our_ip(uint8_t out[4]);
void net_gateway(uint8_t out[4]);
/* Next hop for dst: gateway when off-link, else dst itself. */
void net_route_ip(const uint8_t ip[4], uint8_t next_hop[4]);

/* DNS resolver (Phase 9 inc.1), driven by SYS_NSLOOKUP. */
int  net_dns_begin(const char *name, const uint8_t server_ip[4],
                   const uint8_t server_mac[6]);
int  net_dns_poll(uint8_t out_ip[4]);   /* 1 done, 0 waiting, -1 error */
void net_dns_server(uint8_t out[4]);    /* from DHCP, else 0.0.0.0 */

void net_stack_stats(uint32_t *arp_rx, uint32_t *arp_tx,
                     uint32_t *icmp_rx, uint32_t *icmp_tx,
                     uint32_t *udp_rx, uint32_t *udp_tx);
