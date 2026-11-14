#include "bcache.h"
#include "../drivers/ata.h"
#include "prefetch.h"

/* Cache entry. Fully associative; linear scan is cheap at 64 entries. */
struct bcache_entry {
    uint32_t lba;              /* disk LBA, valid only if valid != 0 */
    uint8_t  data[512];        /* cached sector data */
    uint8_t  valid;            /* 1 if entry holds a block */
    uint8_t  prefetched_unused;/* 1 if inserted by prefetcher, not yet hit */
    uint8_t  dirty;            /* reserved: 1 if modified, needs writeback */
    uint8_t  _pad;
    uint32_t lru_seq;          /* last-access sequence number */
};

static struct bcache_entry cache[BCACHE_SIZE];
static uint32_t lru_clock = 0;
static uint64_t stat_hits = 0;
static uint64_t stat_misses = 0;
static uint64_t stat_pollution = 0;

/* Shadow cache: parallel tag array simulating no-prefetch LRU.
 * Used to measure pollution: if real cache misses but shadow hits,
 * a prefetch evicted something useful. */
static struct {
    uint32_t lba;
    uint8_t  valid;
    uint32_t lru_seq;
} shadow[BCACHE_SIZE];
static uint32_t shadow_clock = 0;

/* Demand-access trace ring (Phase 3, increment 5). */
static struct access_record acc_ring[ACCESS_TRACE_RING_SIZE];
static uint32_t acc_head = 0;   /* index of oldest record */
static uint32_t acc_count = 0;  /* number of valid records */
static uint64_t acc_seq = 0;    /* monotonic access counter */

static void access_trace_record(uint32_t lba, int hit)
{
    uint32_t slot;
    if (acc_count < ACCESS_TRACE_RING_SIZE) {
        slot = (acc_head + acc_count) % ACCESS_TRACE_RING_SIZE;
        acc_count++;
    } else {
        /* Ring full: overwrite oldest. */
        slot = acc_head;
        acc_head = (acc_head + 1) % ACCESS_TRACE_RING_SIZE;
    }
    acc_ring[slot].seq = ++acc_seq;
    acc_ring[slot].lba = lba;
    acc_ring[slot].hit = hit ? 1 : 0;
}

int access_trace_drain(struct access_record *buf, int max)
{
    if (!buf || max <= 0)
        return 0;
    uint32_t n = acc_count < (uint32_t)max ? acc_count : (uint32_t)max;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = (acc_head + i) % ACCESS_TRACE_RING_SIZE;
        buf[i] = acc_ring[slot];
    }
    acc_head = (acc_head + n) % ACCESS_TRACE_RING_SIZE;
    acc_count -= n;
    return (int)n;
}

void bcache_init(void)
{
    for (int i = 0; i < BCACHE_SIZE; i++)
        cache[i].valid = 0;
    lru_clock = 0;
    stat_hits = 0;
    stat_misses = 0;
    stat_pollution = 0;
    shadow_clock = 0;
    for (int i = 0; i < BCACHE_SIZE; i++)
        shadow[i].valid = 0;
    acc_head = 0;
    acc_count = 0;
    acc_seq = 0;
}

/* Find entry by LBA. Returns index or -1. */
static int bcache_lookup(uint32_t lba)
{
    for (int i = 0; i < BCACHE_SIZE; i++) {
        if (cache[i].valid && cache[i].lba == lba)
            return i;
    }
    return -1;
}

/* Find victim for eviction: invalid slot first, else LRU. */
static int bcache_victim(void)
{
    int best = -1;
    uint32_t best_seq = 0xFFFFFFFF;
    for (int i = 0; i < BCACHE_SIZE; i++) {
        if (!cache[i].valid)
            return i;
        if (cache[i].lru_seq < best_seq) {
            best_seq = cache[i].lru_seq;
            best = i;
        }
    }
    return best;
}

static void bcache_touch(int idx)
{
    cache[idx].lru_seq = ++lru_clock;
}

/* Shadow cache lookup. Returns 1 if hit, 0 if miss. */
static int shadow_lookup(uint32_t lba)
{
    for (int i = 0; i < BCACHE_SIZE; i++) {
        if (shadow[i].valid && shadow[i].lba == lba) {
            shadow[i].lru_seq = ++shadow_clock;
            return 1;
        }
    }
    return 0;
}

/* Shadow cache insert (LRU eviction, no prefetch). */
static void shadow_insert(uint32_t lba)
{
    int best = -1;
    uint32_t best_seq = 0xFFFFFFFF;
    for (int i = 0; i < BCACHE_SIZE; i++) {
        if (!shadow[i].valid) { best = i; break; }
        if (shadow[i].lru_seq < best_seq) {
            best_seq = shadow[i].lru_seq;
            best = i;
        }
    }
    shadow[best].lba = lba;
    shadow[best].valid = 1;
    shadow[best].lru_seq = ++shadow_clock;
}

int bcache_read(uint32_t lba, uint8_t *buf)
{
    int idx = bcache_lookup(lba);
    int hit = (idx >= 0);
    if (hit) {
        /* Hit: copy from cache. */
        for (int i = 0; i < 512; i++)
            buf[i] = cache[idx].data[i];
        if (cache[idx].prefetched_unused)
            prefetch_note_outcome(1);  /* consumed a prefetch */
        cache[idx].prefetched_unused = 0;
        bcache_touch(idx);
        stat_hits++;
    } else {
        /* Miss: check shadow for pollution, then read from disk. */
        if (shadow_lookup(lba))
            stat_pollution++;
        shadow_insert(lba);

        uint8_t tmp[512];
        if (ata_read(lba, 1, tmp) != 0)
            return -1;

        int v = bcache_victim();
        if (cache[v].valid && cache[v].prefetched_unused)
            prefetch_note_outcome(0);  /* prefetch evicted unused */
        cache[v].lba = lba;
        for (int i = 0; i < 512; i++) {
            cache[v].data[i] = tmp[i];
            buf[i] = tmp[i];
        }
        cache[v].valid = 1;
        cache[v].prefetched_unused = 0;
        cache[v].dirty = 0;
        bcache_touch(v);
        stat_misses++;
    }

    /* Record the demand access for offline prefetch training, then
     * notify the prefetch policy (may issue prefetches). */
    access_trace_record(lba, hit);
    prefetch_on_access(lba, hit);
    return 0;
}

void bcache_prefetch(uint32_t lba)
{
    /* Skip if already cached. */
    if (bcache_lookup(lba) >= 0)
        return;

    uint8_t tmp[512];
    if (ata_read(lba, 1, tmp) != 0)
        return;  /* best-effort: ignore errors */

    bcache_prefetch_insert(lba, tmp);
}

uint64_t bcache_pollution_misses(void) { return stat_pollution; }

void bcache_prefetch_insert(uint32_t lba, const uint8_t *data)
{
    /* Don't duplicate: if already cached, just refresh LRU. */
    int idx = bcache_lookup(lba);
    if (idx >= 0) {
        bcache_touch(idx);
        return;
    }

    int v = bcache_victim();
    if (cache[v].valid && cache[v].prefetched_unused)
        prefetch_note_outcome(0);  /* prefetch evicted unused */
    /* Prefetches go to the LRU end: use a sequence number older than any
     * real access so a bad prefetch doesn't evict useful data. */
    cache[v].lba = lba;
    for (int i = 0; i < 512; i++)
        cache[v].data[i] = data[i];
    cache[v].valid = 1;
    cache[v].prefetched_unused = 1;
    cache[v].dirty = 0;
    /* Place at LRU end: seq = min existing seq (or 0 if empty). */
    uint32_t min_seq = 0xFFFFFFFF;
    for (int i = 0; i < BCACHE_SIZE; i++) {
        if (cache[i].valid && i != v && cache[i].lru_seq < min_seq)
            min_seq = cache[i].lru_seq;
    }
    cache[v].lru_seq = (min_seq == 0xFFFFFFFF) ? 0 : min_seq - 1;
}

/* Insert a block at the MRU end, marked prefetched. See bcache.h. */
void bcache_prefetch_insert_mru(uint32_t lba, const uint8_t *data)
{
    int idx = bcache_lookup(lba);
    if (idx >= 0) {
        bcache_touch(idx);
        return;
    }

    int v = bcache_victim();
    if (cache[v].valid && cache[v].prefetched_unused)
        prefetch_note_outcome(0);  /* prefetch evicted unused */
    cache[v].lba = lba;
    for (int i = 0; i < 512; i++)
        cache[v].data[i] = data[i];
    cache[v].valid = 1;
    cache[v].prefetched_unused = 1;
    cache[v].dirty = 0;
    bcache_touch(v);  /* MRU end: newest sequence number */
}

void bcache_prefetch_mru(uint32_t lba)
{
    if (bcache_lookup(lba) >= 0)
        return;

    uint8_t tmp[512];
    if (ata_read(lba, 1, tmp) != 0)
        return;  /* best-effort: ignore errors */

    bcache_prefetch_insert_mru(lba, tmp);
}

void bcache_invalidate(void)
{
    for (int i = 0; i < BCACHE_SIZE; i++)
        cache[i].valid = 0;
}

uint64_t bcache_hits(void)   { return stat_hits; }
uint64_t bcache_misses(void) { return stat_misses; }
