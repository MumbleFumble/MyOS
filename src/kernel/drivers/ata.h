#pragma once
#include <stdint.h>

/* Read `count` 512-byte sectors starting at LBA `lba` from the primary
 * master ATA device into `buf`.
 * Returns 0 on success, -1 if the drive is absent or a read error occurs. */
int ata_read(uint32_t lba, uint32_t count, uint8_t *buf);

/* Write `count` 512-byte sectors from `buf` starting at LBA `lba`
 * (Phase 6 inc.1). Mirrors ata_read: ata_write() takes the disk
 * mutex, ata_write_locked() is for callers already holding it. A
 * FLUSH CACHE is issued before returning so a completed write is
 * durable. Returns 0 on success, -1 on error. */
int ata_write(uint32_t lba, uint32_t count, const uint8_t *buf);
int ata_write_locked(uint32_t lba, uint32_t count, const uint8_t *buf);

/* Total LBA28 sectors reported by IDENTIFY DEVICE (0 = unknown). */
uint32_t ata_total_sectors(void);

/* Write-path counters for SYS_WSTAT. */
void ata_write_stats(uint64_t *dma_sectors, uint64_t *dma_errors,
                     uint64_t *pio_sectors);

/* Disk mutex (Phase 4 inc.2): serialises the single command channel
 * between demand readers and the prefetch worker. ata_read() takes it
 * internally; the worker takes it explicitly so it can re-check the
 * cache after acquiring it (a target that was in flight when queued
 * is usually cached by the time the worker gets the disk). */
void ata_lock(void);
void ata_unlock(void);
/* ata_read() body for callers already holding the disk mutex. */
int ata_read_locked(uint32_t lba, uint32_t count, uint8_t *buf);

/* Returns 1 if a primary master ATA disk is detected, 0 otherwise. */
int ata_detect(void);

/* Phase 4 inc.1: switch sector waits from polling to IRQ14-driven
 * blocking (the reading task sleeps until the drive signals DRQ).
 * Call after irq_init() and sched_init(); polling remains the fallback
 * for the idle task and whenever an interrupt is lost. */
void ata_enable_irq(void);

/* Phase 5 inc.1: find the PCI bus-master IDE controller and set up
 * DMA reads (PRDT + bounce buffer). Safe to call when no BMIDE is
 * present: the driver then stays on the PIO path. Call before
 * ata_enable_irq(); DMA is only used once IRQ mode is on. */
void ata_dma_init(void);

/* DMA/PIO transfer counters for SYS_DMA_STAT. */
void ata_dma_stats(uint64_t *dma_sectors, uint64_t *dma_errors,
                   uint64_t *pio_sectors);

/* -----------------------------------------------------------------------
 * I/O telemetry (Phase 3, increment 1).
 *
 * Every ata_read() is recorded in a ring buffer: start/end ticks, LBA,
 * sector count, latency in ticks, and result code. Userspace drains via
 * SYS_IO_DRAIN.
 * ----------------------------------------------------------------------- */
#define IO_TRACE_RING_SIZE 256

struct io_record {
    uint64_t tsc_start;   /* TSC when request started */
    uint64_t tsc_end;     /* TSC when request completed */
    uint32_t lba;         /* starting LBA */
    uint32_t count;       /* sector count */
    uint32_t latency_us;  /* (tsc_end - tsc_start) in microseconds */
    int32_t  result;      /* 0 = success, -1 = error */
};                        /* 32 bytes */

/* Drain up to max records (oldest first). Returns count copied. */
int io_trace_drain(struct io_record *buf, int max);
