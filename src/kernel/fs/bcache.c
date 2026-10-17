#include "bcache.h"
#include "../drivers/ata.h"

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

void bcache_init(void)
{
    for (int i = 0; i < BCACHE_SIZE; i++)
        cache[i].valid = 0;
    lru_clock = 0;
    stat_hits = 0;
    stat_misses = 0;
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

int bcache_read(uint32_t lba, uint8_t *buf)
{
    int idx = bcache_lookup(lba);
    if (idx >= 0) {
        /* Hit: copy from cache. */
        for (int i = 0; i < 512; i++)
            buf[i] = cache[idx].data[i];
        cache[idx].prefetched_unused = 0;  /* consumed a prefetch */
        bcache_touch(idx);
        stat_hits++;
        return 0;
    }

    /* Miss: read from disk, then insert. */
    uint8_t tmp[512];
    if (ata_read(lba, 1, tmp) != 0)
        return -1;

    int v = bcache_victim();
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
    return 0;
}

void bcache_prefetch_insert(uint32_t lba, const uint8_t *data)
{
    /* Don't duplicate: if already cached, just refresh LRU. */
    int idx = bcache_lookup(lba);
    if (idx >= 0) {
        bcache_touch(idx);
        return;
    }

    int v = bcache_victim();
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

void bcache_invalidate(void)
{
    for (int i = 0; i < BCACHE_SIZE; i++)
        cache[i].valid = 0;
}

uint64_t bcache_hits(void)   { return stat_hits; }
uint64_t bcache_misses(void) { return stat_misses; }
