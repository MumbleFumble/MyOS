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

/* Insert a block into the cache, marked as prefetched (prefetched_unused=1).
 * Used by the prefetch policy. Evicts LRU entry if full. */
void bcache_prefetch_insert(uint32_t lba, const uint8_t *data);

/* Invalidate all entries (e.g., on unmount). */
void bcache_invalidate(void);

/* Prefetch one block: read from disk and insert as prefetched.
 * No-op if already cached. Used by prefetch policies. */
void bcache_prefetch(uint32_t lba);

/* Pollution stats: demand misses that would have hit without prefetching
 * (measured via shadow cache). */
uint64_t bcache_pollution_misses(void);

/* Hit/miss statistics. */
uint64_t bcache_hits(void);
uint64_t bcache_misses(void);
