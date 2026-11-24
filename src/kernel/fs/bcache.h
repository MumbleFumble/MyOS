#pragma once
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Block cache (Phase 3, pivot to learned prefetching).
 *
 * Fully-associative cache of 512-byte disk blocks, sitting between diskfs
 * and ata_read. LRU eviction. Each entry tracks whether it was prefetched
 * (for accuracy metrics) and has a dirty bit reserved for future writes.
 *
 * Cache size is configurable at compile time; sweep 16/64/256 for evaluation.
 * ----------------------------------------------------------------------- */
#ifndef BCACHE_SIZE
#define BCACHE_SIZE 64
#endif

/* Initialize the cache. Must be called before first use. */
void bcache_init(void);

/* Read one 512-byte sector at LBA into buf.
 * Returns 0 on success (hit or miss+fill), -1 on disk error.
 * On hit: buf filled from cache, hits++.
 * On miss: reads from disk via ata_read, inserts into cache (LRU eviction
 * if full), buf filled, misses++. */
int bcache_read(uint32_t lba, uint8_t *buf);

/* As bcache_read, with the demand's file context (Phase 3 inc.9):
 * file_id identifies the file being read (diskfs directory index + 1,
 * 0 = no file context) and file_off is the sector index within the
 * file. The context is recorded in the access trace and passed to the
 * prefetch policy, whose LBA-only view distorts per-file streams. */
int bcache_read_ctx(uint32_t lba, uint8_t *buf, uint32_t file_id,
                    uint32_t file_off);

/* Write one sector through the cache (Phase 6 inc.2, write-back):
 * the entry is updated or inserted and marked dirty; data reaches
 * disk on eviction or bcache_sync(). Returns 0 or -1. */
int bcache_write(uint32_t lba, const uint8_t *data);

/* Flush every dirty entry to disk. 0 on success, -1 if any write
 * failed (failed entries stay dirty). */
int bcache_sync(void);

/* Dirty entries currently held, and entries written back so far. */
uint64_t bcache_dirty_count(void);
uint64_t bcache_writebacks(void);

/* Insert a block into the cache, marked as prefetched (prefetched_unused=1).
 * Caller must hold the ATA disk mutex (evicting a dirty victim
 * writes it back).
 * Used by the prefetch policy. Evicts LRU entry if full. */
void bcache_prefetch_insert(uint32_t lba, const uint8_t *data);

/* Invalidate all entries (e.g., on unmount). */
void bcache_invalidate(void);

/* Invalidate every entry in [lba, lba+count) (Phase 13 inc.8),
 * for when the filesystem frees a sector range. Refuses (-1,
 * drops nothing) if any entry in the range is dirty or the
 * range starts at sector 0. Returns entries dropped. */
int bcache_invalidate_range(uint32_t lba, uint32_t count);

/* Cache-state probe (Phase 13 inc.8): bit0 valid, bit1 dirty,
 * bit2 prefetched-but-unused; 0 if the LBA is not cached. */
int bcache_probe(uint32_t lba);

/* Async prefetch (Phase 4 inc.2): queue LBA for the prefetch worker
 * instead of reading it synchronously. mru=1 inserts at the MRU end
 * (learned policy), mru=0 at the LRU end (baselines). No-op if the
 * block is already cached or already queued; drops (counted) when the
 * queue is full. */
void bcache_prefetch_enqueue(uint32_t lba, int mru);

/* Queue counters: enqueued, dropped (queue full), completed reads. */
void bcache_prefetch_stats(uint64_t *queued, uint64_t *dropped,
                           uint64_t *done);

/* Prefetch worker task body; created by kernel_main via task_create
 * after ata_enable_irq(). */
void bcache_prefetch_worker(void);

/* Prefetch one block: read from disk and insert as prefetched.
 * No-op if already cached. Used by prefetch policies. */
void bcache_prefetch(uint32_t lba);

/* Prefetch one block, inserting at the MRU end so the block survives
 * until its demand arrives. Used by the learned policy, whose score is
 * a confidence estimate (Phase 3 inc.6 finding: LRU-end insertion makes
 * each new prefetch the next eviction victim under a full cache). */
void bcache_prefetch_mru(uint32_t lba);

/* Pollution stats: demand misses that would have hit without prefetching
 * (measured via shadow cache). */
uint64_t bcache_pollution_misses(void);

/* Hit/miss statistics. */
uint64_t bcache_hits(void);
uint64_t bcache_misses(void);

/* Demand-access trace (Phase 3, increment 5): every bcache_read demand
 * access (not prefetches) is recorded in a ring, drained by
 * SYS_ACCESS_DRAIN. This is the training data for the learned prefetch
 * policy: the offline simulator replays the demand stream through
 * candidate policies. */
struct access_record {
    uint64_t seq;     /* monotonic access counter */
    uint32_t lba;     /* requested block */
    uint32_t hit;     /* 1 if the access hit the cache */
    uint32_t file_id; /* file being read (0 = no file context) */
    uint32_t file_off;/* sector index within the file */
};

#define ACCESS_TRACE_RING_SIZE 4096

/* Copy up to max records (oldest first) into buf and consume them.
 * Returns the number of records written. */
int access_trace_drain(struct access_record *buf, int max);
