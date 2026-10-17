#pragma once
#include <stdint.h>

/* Read `count` 512-byte sectors starting at LBA `lba` from the primary
 * master ATA device into `buf`.
 * Returns 0 on success, -1 if the drive is absent or a read error occurs. */
int ata_read(uint32_t lba, uint32_t count, uint8_t *buf);

/* Returns 1 if a primary master ATA disk is detected, 0 otherwise. */
int ata_detect(void);

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
