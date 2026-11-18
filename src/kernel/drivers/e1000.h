#pragma once
#include <stdint.h>

/* Intel 82540EM (QEMU "e1000") gigabit NIC driver — Phase 7.
 * Polling-mode descriptor rings over MMIO; see e1000.c. */

/* Probe + initialise the NIC. Returns 0 if present and up, -1 if no
 * supported NIC exists (all other boot paths tolerate its absence). */
int  e1000_init(void);
int  e1000_present(void);
void e1000_mac_addr(uint8_t out[6]);
int  e1000_link_up(void);

/* Send one raw Ethernet frame (14-byte header included). 0 or -1. */
int  e1000_send(const uint8_t *frame, uint16_t len);
/* Poll for a received frame: copies it to out (2048 bytes), sets
 * *len_out, returns 1; returns 0 when the ring is empty. */
int  e1000_recv(uint8_t *out, uint16_t *len_out);

/* Kernel task body: drains the RX ring and logs frames to serial. */
void e1000_rx_task(void);

/* Send the fixed test frame (broadcast, ethertype 0x88B5). */
int  e1000_send_hello(void);

void e1000_stats(uint64_t *rx_frames, uint64_t *tx_frames);
