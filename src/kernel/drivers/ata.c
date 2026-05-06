#include "ata.h"
#include "../arch/port_io.h"

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

int ata_read(uint32_t lba, uint32_t count, uint8_t *buf)
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
