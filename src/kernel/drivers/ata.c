#include "ata.h"
#include "../arch/port_io.h"
#include "../arch/timer.h"

/* Primary ATA channel I/O ports */
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECT_COUNT  0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7

/* Status register bits */
#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

/* Commands */
#define ATA_CMD_READ_PIO 0x20

static void ata_delay400ns(void)
{
    /* Read the alternate status port 4 times = ~400 ns delay */
    inb(0x3F6); inb(0x3F6); inb(0x3F6); inb(0x3F6);
}

static int ata_wait_not_busy(void)
{
    int timeout = 100000;
    while ((inb(ATA_STATUS) & ATA_SR_BSY) && --timeout);
    return timeout > 0 ? 0 : -1;
}

static int ata_wait_drq(void)
{
    int timeout = 100000;
    uint8_t status;
    do {
        status = inb(ATA_STATUS);
        if (status & ATA_SR_ERR) return -1;
        if (status & ATA_SR_DRQ) return 0;
    } while (--timeout);
    return -1;
}

int ata_detect(void)
{
    /* Select drive 0 (master) with LBA mode */
    outb(ATA_DRIVE_HEAD, 0xE0);
    ata_delay400ns();

    /* If status is 0xFF the bus is floating (no drive) */
    uint8_t status = inb(ATA_STATUS);
    if (status == 0xFF) return 0;

    /* Wait for BSY to clear */
    if (ata_wait_not_busy() != 0) return 0;
    return 1;
}

/* Forward declaration: defined below ata_read(). */
static void io_trace_record(uint32_t lba, uint32_t count,
                            uint32_t tick_start, uint32_t tick_end,
                            int result);
static int ata_read_inner(uint32_t lba, uint32_t count, uint8_t *buf);

int ata_read(uint32_t lba, uint32_t count, uint8_t *buf)
{
    uint32_t tick_start = timer_ticks();
    int result = ata_read_inner(lba, count, buf);
    uint32_t tick_end = timer_ticks();

    io_trace_record(lba, count, tick_start, tick_end, result);
    return result;
}

/* -----------------------------------------------------------------------
 * I/O telemetry ring (Phase 3, increment 1).
 *
 * Records every ata_read(): start/end ticks, LBA, sector count, latency,
 * and result. Drained by SYS_IO_DRAIN.
 * ----------------------------------------------------------------------- */
static struct io_record io_ring[IO_TRACE_RING_SIZE];
static uint32_t io_head  = 0;   /* index of oldest record */
static uint32_t io_count = 0;   /* number of valid records */

static void io_trace_record(uint32_t lba, uint32_t count,
                            uint32_t tick_start, uint32_t tick_end,
                            int result)
{
    __asm__ volatile("cli");
    uint32_t slot;
    if (io_count < IO_TRACE_RING_SIZE) {
        slot = (io_head + io_count) % IO_TRACE_RING_SIZE;
        io_count++;
    } else {
        /* Ring full: overwrite oldest. */
        slot = io_head;
        io_head = (io_head + 1) % IO_TRACE_RING_SIZE;
    }
    io_ring[slot].tick_start = (uint64_t)tick_start;
    io_ring[slot].tick_end   = (uint64_t)tick_end;
    io_ring[slot].lba        = lba;
    io_ring[slot].count      = count;
    io_ring[slot].latency    = tick_end - tick_start;
    io_ring[slot].result     = (int32_t)result;
    __asm__ volatile("sti");
}

int io_trace_drain(struct io_record *buf, int max)
{
    if (!buf || max <= 0)
        return 0;

    __asm__ volatile("cli");
    uint32_t n = io_count < (uint32_t)max ? io_count : (uint32_t)max;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = (io_head + i) % IO_TRACE_RING_SIZE;
        buf[i] = io_ring[slot];
    }
    io_head   = (io_head + n) % IO_TRACE_RING_SIZE;
    io_count -= n;
    __asm__ volatile("sti");

    return (int)n;
}

static int ata_read_inner(uint32_t lba, uint32_t count, uint8_t *buf)
{
    if (count == 0) return 0;

    if (ata_wait_not_busy() != 0) return -1;

    /* Select drive 0, LBA mode, upper 4 bits of LBA */
    outb(ATA_DRIVE_HEAD, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_delay400ns();

    outb(ATA_SECT_COUNT, (uint8_t)count);
    outb(ATA_LBA_LO,     (uint8_t)(lba        & 0xFF));
    outb(ATA_LBA_MID,    (uint8_t)((lba >> 8)  & 0xFF));
    outb(ATA_LBA_HI,     (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND,    ATA_CMD_READ_PIO);

    for (uint32_t s = 0; s < count; s++) {
        if (ata_wait_drq() != 0) return -1;

        /* Read 256 16-bit words = 512 bytes */
        uint16_t *dst = (uint16_t *)(buf + s * 512);
        for (int i = 0; i < 256; i++)
            dst[i] = inw(ATA_DATA);
    }
    return 0;
}
