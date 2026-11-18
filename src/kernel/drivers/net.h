#pragma once
#include <stdint.h>

/* Minimal network stack (Phase 7 inc.2): ARP + IPv4 + ICMP echo.
 * Static IPv4 address 192.168.77.2/24 (the socket-backend test peer
 * plays the network; for a future slirp backend the guest address
 * would be 10.0.2.15). See net.c. */

/* Kernel task body: drains the e1000 RX ring into net_input(). */
void net_rx_task(void);

/* Dispatch one received Ethernet frame (ARP / IPv4 / log others). */
void net_input(const uint8_t *frame, uint16_t len);

/* ARP cache: 1 if ip is known (mac_out filled), else 0. */
int  net_arp_lookup(const uint8_t ip[4], uint8_t mac_out[6]);
/* Broadcast an ARP request for ip (async; the reply fills the cache). */
void net_arp_request(const uint8_t ip[4]);

void net_stack_stats(uint32_t *arp_rx, uint32_t *arp_tx,
                     uint32_t *icmp_rx, uint32_t *icmp_tx);
