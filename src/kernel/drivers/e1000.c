/* e1000.c — Intel 82540EM NIC driver (Phase 7, increment 1).
 *
 * Raw Ethernet over descriptor rings, polled (no IRQ yet): a kernel
 * task drains RX; TX is synchronous. Buffers and rings are kernel
 * statics (identity-mapped low memory, like the ATA DMA bounce
 * buffer), so physical addresses come from vmm_virt_to_phys.
 * The MMIO BAR sits high (~0xFEBC0000 under QEMU) and is mapped
 * explicitly with cache-disable, since boot only identity-maps the
 * first 16 MB.
 */
#include "e1000.h"
#include "../arch/port_io.h"
#include "../arch/timer.h"
#include "../proc/sched.h"
#include "../mem/vmm.h"

/* ---- PCI (local copies of ata.c's helpers; dedup is future work) ---- */
static uint32_t e1000_pci_read32(uint32_t dev, uint32_t fn, uint32_t off)
{
    uint32_t addr = 0x80000000u | (dev << 11) | (fn << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}
static void e1000_pci_write32(uint32_t dev, uint32_t fn, uint32_t off,
                              uint32_t val)
{
    uint32_t addr = 0x80000000u | (dev << 11) | (fn << 8) | (off & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

/* ---- Registers (byte offsets into BAR0) ---- */
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMS    0x00D0
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL0   0x5400
#define REG_RAH0   0x5404

#define CTRL_SLU  (1u << 6)
#define CTRL_ASDE (1u << 5)
#define CTRL_RST  (1u << 26)
#define STATUS_LU (1u << 1)
#define RCTL_EN   (1u << 1)
#define RCTL_UPE  (1u << 3)
#define RCTL_MPE  (1u << 4)
#define RCTL_BAM  (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN   (1u << 1)
#define TCTL_PSP  (1u << 3)

#define RX_COUNT 128
#define TX_COUNT 64
#define RX_BUF_SIZE 2048

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

static struct e1000_rx_desc rx_ring[RX_COUNT] __attribute__((aligned(16)));
static struct e1000_tx_desc tx_ring[TX_COUNT] __attribute__((aligned(16)));
static uint8_t rx_bufs[RX_COUNT][RX_BUF_SIZE];
static uint8_t tx_buf[RX_BUF_SIZE];

static volatile uint32_t *mmio = 0;
static int      net_present = 0;
static uint8_t  net_mac[6];
static uint32_t rx_next = 0;
static uint32_t tx_next = 0;
static uint64_t stat_rx = 0, stat_tx = 0;

static uint32_t reg_read(uint32_t off) { return mmio[off / 4]; }
static void reg_write(uint32_t off, uint32_t val) { mmio[off / 4] = val; }

static uint64_t phys_of(const void *p)
{
    return vmm_virt_to_phys(vmm_current_cr3(), (uint64_t)p);
}

static void delay_loops(uint32_t n)
{
    for (volatile uint32_t i = 0; i < n; i++) { }
}

/* ---- tiny serial logger (COM1), self-contained like exceptions.c ---- */
static void net_putc(char c)
{
    while (!(inb(0x3FD) & 0x20)) { }
    outb(0x3F8, (uint8_t)c);
}
static void net_puts(const char *s) { while (*s) net_putc(*s++); }
static void net_putu(uint32_t v)
{
    char b[12]; int i = 0;
    if (!v) { net_putc('0'); return; }
    while (v) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) net_putc(b[--i]);
}
static void net_puthex(uint32_t v, int digits)
{
    static const char h[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--)
        net_putc(h[(v >> (i * 4)) & 0xF]);
}

static void net_log_frame(const uint8_t *f, uint16_t len)
{
    net_puts("[net] rx len=");
    net_putu(len);
    if (len >= 14) {
        net_puts(" type=0x");
        net_puthex(((uint32_t)f[12] << 8) | f[13], 4);
        net_puts(" data=\"");
        uint16_t n = len - 14;
        if (n > 48) n = 48;
        for (uint16_t i = 0; i < n; i++) {
            uint8_t c = f[14 + i];
            net_putc((c >= 32 && c < 127) ? (char)c : '.');
        }
        net_putc('"');
    }
    net_puts("\r\n");
}

/* EEPROM read via EERD (fallback MAC source). */
static uint16_t eeprom_read(uint8_t addr)
{
    reg_write(REG_EERD, 1u | ((uint32_t)addr << 8));
    uint32_t v;
    uint32_t guard = 100000;
    do { v = reg_read(REG_EERD); } while (!(v & (1u << 4)) && --guard);
    return (uint16_t)(v >> 16);
}

int e1000_init(void)
{
    uint32_t fdev = 32, ffn = 0;
    for (uint32_t dev = 0; dev < 32; dev++) {
        for (uint32_t fn = 0; fn < 8; fn++) {
            uint32_t id = e1000_pci_read32(dev, fn, 0);
            if ((id & 0xFFFF) == 0xFFFF) {
                if (fn == 0) break;
                continue;
            }
            if (id == ((0x100Eu << 16) | 0x8086u)) { fdev = dev; ffn = fn; break; }
        }
        if (fdev != 32) break;
    }
    if (fdev == 32)
        return -1;

    uint32_t bar0 = e1000_pci_read32(fdev, ffn, 0x10) & 0xFFFFFFF0u;
    if (!bar0)
        return -1;
    /* Memory space + bus mastering. */
    uint32_t cmd = e1000_pci_read32(fdev, ffn, 4);
    if ((cmd & 0x6) != 0x6)
        e1000_pci_write32(fdev, ffn, 4, cmd | 0x6);

    /* Map the 128 KiB MMIO window (cache-disabled) at VA == PA. */
    for (uint32_t off = 0; off < 128 * 1024; off += 4096)
        vmm_map_page(bar0 + off, bar0 + off, PAGE_RW | 0x10 /* PCD */);
    mmio = (volatile uint32_t *)(uint64_t)bar0;

    /* Reset. */
    reg_write(REG_IMS, 0);
    (void)reg_read(REG_ICR);
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RST);
    uint32_t guard = 1000000;
    while ((reg_read(REG_CTRL) & CTRL_RST) && --guard) { }
    reg_write(REG_IMS, 0);
    (void)reg_read(REG_ICR);
    delay_loops(10000);

    /* MAC: RAL/RAH first, EEPROM as fallback. */
    uint32_t ral = reg_read(REG_RAL0);
    uint32_t rah = reg_read(REG_RAH0);
    net_mac[0] = (uint8_t)ral;
    net_mac[1] = (uint8_t)(ral >> 8);
    net_mac[2] = (uint8_t)(ral >> 16);
    net_mac[3] = (uint8_t)(ral >> 24);
    net_mac[4] = (uint8_t)rah;
    net_mac[5] = (uint8_t)(rah >> 8);
    if (!(ral | rah)) {
        uint16_t w0 = eeprom_read(0), w1 = eeprom_read(1), w2 = eeprom_read(2);
        net_mac[0] = (uint8_t)w0; net_mac[1] = (uint8_t)(w0 >> 8);
        net_mac[2] = (uint8_t)w1; net_mac[3] = (uint8_t)(w1 >> 8);
        net_mac[4] = (uint8_t)w2; net_mac[5] = (uint8_t)(w2 >> 8);
    }

    /* No multicast filtering. */
    for (int i = 0; i < 128; i++)
        reg_write(REG_MTA + (uint32_t)i * 4, 0);

    /* RX ring. */
    for (int i = 0; i < RX_COUNT; i++) {
        rx_ring[i].addr = phys_of(rx_bufs[i]);
        rx_ring[i].status = 0;
    }
    uint64_t rp = phys_of(rx_ring);
    reg_write(REG_RDBAL, (uint32_t)rp);
    reg_write(REG_RDBAH, (uint32_t)(rp >> 32));
    reg_write(REG_RDLEN, RX_COUNT * 16);
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, RX_COUNT - 1);
    reg_write(REG_RCTL, RCTL_EN | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_SECRC);

    /* TX ring. */
    for (int i = 0; i < TX_COUNT; i++)
        tx_ring[i].status = 0;
    uint64_t tp = phys_of(tx_ring);
    reg_write(REG_TDBAL, (uint32_t)tp);
    reg_write(REG_TDBAH, (uint32_t)(tp >> 32));
    reg_write(REG_TDLEN, TX_COUNT * 16);
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    reg_write(REG_TIPG, 0x0060200A);
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP);

    /* Link up. */
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    net_present = 1;
    return 0;
}

int e1000_present(void) { return net_present; }

void e1000_mac_addr(uint8_t out[6])
{
    for (int i = 0; i < 6; i++) out[i] = net_mac[i];
}

int e1000_link_up(void)
{
    return net_present && (reg_read(REG_STATUS) & STATUS_LU);
}

int e1000_send(const uint8_t *frame, uint16_t len)
{
    if (!net_present || len > RX_BUF_SIZE)
        return -1;
    for (uint16_t i = 0; i < len; i++)
        tx_buf[i] = frame[i];
    struct e1000_tx_desc *d = &tx_ring[tx_next];
    d->addr = phys_of(tx_buf);
    d->length = len;
    d->cso = 0;
    d->css = 0;
    d->special = 0;
    d->status = 0;
    d->cmd = 0x0B;   /* EOP | IFCS | RS */
    tx_next = (tx_next + 1) % TX_COUNT;
    reg_write(REG_TDT, tx_next);
    uint32_t guard = 1000000;
    while (!(*(volatile uint8_t *)&d->status & 1) && --guard) { }
    if (!guard) {
        net_puts("[net] tx timeout tdh=");
        net_putu(reg_read(REG_TDH));
        net_puts(" tdt=");
        net_putu(reg_read(REG_TDT));
        net_puts(" tctl=0x");
        net_puthex(reg_read(REG_TCTL), 8);
        net_puts(" status=0x");
        net_puthex(reg_read(REG_STATUS), 8);
        net_puts("\r\n");
        return -1;
    }
    stat_tx++;
    return 0;
}

int e1000_recv(uint8_t *out, uint16_t *len_out)
{
    if (!net_present)
        return 0;
    struct e1000_rx_desc *d = &rx_ring[rx_next];
    if (!(d->status & 1))
        return 0;
    uint16_t len = d->length;
    if (len > RX_BUF_SIZE)
        len = RX_BUF_SIZE;
    for (uint16_t i = 0; i < len; i++)
        out[i] = rx_bufs[rx_next][i];
    *len_out = len;
    d->status = 0;
    reg_write(REG_RDT, rx_next);
    rx_next = (rx_next + 1) % RX_COUNT;
    stat_rx++;
    return 1;
}

void e1000_rx_task(void)
{
    uint8_t frame[RX_BUF_SIZE];
    uint16_t len;
    for (;;) {
        int got = 0;
        while (e1000_recv(frame, &len)) {
            got = 1;
            net_log_frame(frame, len);
        }
        if (!got)
            sched_block_current((uint64_t)timer_ticks() + 2);
    }
}

int e1000_send_hello(void)
{
    if (!net_present)
        return -1;
    uint8_t f[60];
    for (int i = 0; i < 60; i++)
        f[i] = 0;
    for (int i = 0; i < 6; i++) {
        f[i] = 0xFF;              /* broadcast */
        f[6 + i] = net_mac[i];
    }
    f[12] = 0x88; f[13] = 0xB5;   /* experimental ethertype */
    const char *msg = "MYOSNET-HELLO";
    for (int i = 0; msg[i]; i++)
        f[14 + i] = (uint8_t)msg[i];
    return e1000_send(f, 60);
}

void e1000_stats(uint64_t *rx_frames, uint64_t *tx_frames)
{
    *rx_frames = stat_rx;
    *tx_frames = stat_tx;
}
