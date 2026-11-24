#include "bcache.h"
#include "../drivers/ata.h"
#include "../arch/port_io.h"
#include "prefetch.h"
#include "../proc/sched.h"
#include "../arch/timer.h"

/* The cache is shared between demand tasks and the async prefetch
 * worker (Phase 4 inc.2), and a timer preemption can land mid-update:
 * every critical section over the entry array runs with interrupts
 * masked, using the same save/restore idiom as the I/O trace ring.
 * Disk reads themselves always happen outside the masked window. */
static uint64_t bcache_cli_save(void)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    __asm__ volatile("cli");
    return rflags;
}
static void bcache_restore(uint64_t rflags)
{
    if (rflags & 0x200) __asm__ volatile("sti");
}


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

static void access_trace_record(uint32_t lba, int hit, uint32_t file_id,
                                uint32_t file_off)
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
    acc_ring[slot].file_id = file_id;
    acc_ring[slot].file_off = file_off;
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

/* -----------------------------------------------------------------------
 * Write-back support (Phase 6, increment 2).
 *
 * Entries carry a dirty bit (reserved since Phase 3). Evicting a dirty
 * entry must write it back first, or data is lost. All cache fills go
 * through bcache_clean_victim(), which flushes LRU victims until the
 * victim it returns is safe to overwrite. Callers must hold the ATA
 * disk mutex: the only path that dirties entries (bcache_write) takes
 * it too, so a victim cannot become dirty again between the flush and
 * the fill.
 * ----------------------------------------------------------------------- */
static uint64_t stat_writebacks = 0;

static int bcache_clean_victim(void)
{
    for (;;) {
        uint64_t f = bcache_cli_save();
        int v = bcache_victim();
        if (!cache[v].valid || !cache[v].dirty) {
            bcache_restore(f);
            return v;
        }
        uint32_t vlba = cache[v].lba;
        uint8_t vdata[512];
        for (int i = 0; i < 512; i++)
            vdata[i] = cache[v].data[i];
        bcache_restore(f);
        if (ata_write_locked(vlba, 1, vdata) != 0)
            return -1;
        stat_writebacks++;
        f = bcache_cli_save();
        int idx = bcache_lookup(vlba);
        if (idx >= 0)
            cache[idx].dirty = 0;
        bcache_restore(f);
    }
}

uint64_t bcache_writebacks(void) { return stat_writebacks; }

uint64_t bcache_dirty_count(void)
{
    uint64_t f = bcache_cli_save();
    uint64_t n = 0;
    for (int i = 0; i < BCACHE_SIZE; i++)
        if (cache[i].valid && cache[i].dirty)
            n++;
    bcache_restore(f);
    return n;
}

/* Write one sector through the cache (write-back): the entry is
 * updated/inserted and marked dirty; the data reaches disk on
 * eviction or bcache_sync(). Returns 0 or -1. */
int bcache_write(uint32_t lba, const uint8_t *data)
{
    ata_lock();
    uint64_t f = bcache_cli_save();
    int idx = bcache_lookup(lba);
    if (idx >= 0) {
        for (int i = 0; i < 512; i++)
            cache[idx].data[i] = data[i];
        cache[idx].dirty = 1;
        cache[idx].prefetched_unused = 0;
        bcache_touch(idx);
        bcache_restore(f);
        ata_unlock();
        return 0;
    }
    bcache_restore(f);
    if (bcache_clean_victim() < 0) {
        ata_unlock();
        return -1;
    }
    f = bcache_cli_save();
    int v = bcache_victim();
    if (cache[v].valid && cache[v].prefetched_unused)
        prefetch_note_outcome(cache[v].lba, 0);  /* prefetch evicted unused */
    cache[v].lba = lba;
    for (int i = 0; i < 512; i++)
        cache[v].data[i] = data[i];
    cache[v].valid = 1;
    cache[v].prefetched_unused = 0;
    cache[v].dirty = 1;
    bcache_touch(v);
    bcache_restore(f);
    ata_unlock();
    return 0;
}

/* Flush every dirty entry to disk. Returns 0, or -1 if any write
 * failed (failed entries stay dirty). */
int bcache_sync(void)
{
    ata_lock();
    int rc = 0;
    for (int i = 0; i < BCACHE_SIZE; i++) {
        uint64_t f = bcache_cli_save();
        int dirty = cache[i].valid && cache[i].dirty;
        uint32_t lba = cache[i].lba;
        uint8_t data[512];
        if (dirty)
            for (int j = 0; j < 512; j++)
                data[j] = cache[i].data[j];
        bcache_restore(f);
        if (!dirty)
            continue;
        if (ata_write_locked(lba, 1, data) != 0) {
            rc = -1;
            continue;
        }
        stat_writebacks++;
        f = bcache_cli_save();
        int idx = bcache_lookup(lba);
        if (idx >= 0)
            cache[idx].dirty = 0;
        bcache_restore(f);
    }
    ata_unlock();
    return rc;
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
    return bcache_read_ctx(lba, buf, 0, 0);
}

int bcache_read_ctx(uint32_t lba, uint8_t *buf, uint32_t file_id,
                    uint32_t file_off)
{
    uint64_t f = bcache_cli_save();
    int idx = bcache_lookup(lba);
    if (idx >= 0) {
        /* Hit: copy from cache. */
        for (int i = 0; i < 512; i++)
            buf[i] = cache[idx].data[i];
        if (cache[idx].prefetched_unused)
            prefetch_note_outcome(lba, 1);  /* consumed a prefetch */
        cache[idx].prefetched_unused = 0;
        bcache_touch(idx);
        stat_hits++;
        bcache_restore(f);
        access_trace_record(lba, 1, file_id, file_off);
        prefetch_on_access(lba, 1, file_id, file_off);
        return 0;
    }
    bcache_restore(f);

    /* Miss: check shadow for pollution. */
    f = bcache_cli_save();
    if (shadow_lookup(lba))
        stat_pollution++;
    shadow_insert(lba);
    bcache_restore(f);

    /* Double-checked demand read (Phase 4 inc.2): take the disk mutex
     * and look AGAIN before reading. The prefetch worker holds the
     * mutex in batches and runs ahead of the demand frontier, so this
     * block may have arrived while we waited for the mutex. Without
     * the re-check the demand would re-read it from disk anyway, and
     * no async prefetch could ever convert into a saved wait (the
     * lock handoff ping-pong otherwise keeps the worker exactly one
     * block behind the frontier forever). */
    uint8_t tmp[512];
    ata_lock();
    f = bcache_cli_save();
    idx = bcache_lookup(lba);
    if (idx >= 0) {
        /* Arrived via the worker while we waited: serve from cache. */
        for (int i = 0; i < 512; i++)
            buf[i] = cache[idx].data[i];
        if (cache[idx].prefetched_unused)
            prefetch_note_outcome(lba, 1);  /* consumed a prefetch */
        cache[idx].prefetched_unused = 0;
        bcache_touch(idx);
        stat_hits++;
        bcache_restore(f);
        ata_unlock();
        access_trace_record(lba, 1, file_id, file_off);
        prefetch_on_access(lba, 1, file_id, file_off);
        return 0;
    }
    bcache_restore(f);

    if (ata_read_locked(lba, 1, tmp) != 0) {
        ata_unlock();
        return -1;
    }

    if (bcache_clean_victim() < 0) {
        ata_unlock();
        return -1;
    }
    f = bcache_cli_save();
    int v = bcache_victim();
    if (cache[v].valid && cache[v].prefetched_unused)
        prefetch_note_outcome(cache[v].lba, 0);  /* prefetch evicted unused */
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
    bcache_restore(f);
    ata_unlock();

    /* Record the demand access for offline prefetch training, then
     * notify the prefetch policy (enqueues async prefetches). */
    access_trace_record(lba, 0, file_id, file_off);
    prefetch_on_access(lba, 0, file_id, file_off);
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

    ata_lock();
    bcache_prefetch_insert(lba, tmp);
    ata_unlock();
}

uint64_t bcache_pollution_misses(void) { return stat_pollution; }

void bcache_prefetch_insert(uint32_t lba, const uint8_t *data)
{
    uint64_t f = bcache_cli_save();
    /* Don't duplicate: if already cached, just refresh LRU. */
    int idx = bcache_lookup(lba);
    if (idx >= 0) {
        bcache_touch(idx);
        bcache_restore(f);
        return;
    }

    bcache_restore(f);
    if (bcache_clean_victim() < 0)
        return;  /* write-back failed: skip this prefetch */
    f = bcache_cli_save();
    if (bcache_lookup(lba) >= 0) { bcache_restore(f); return; }
    int v = bcache_victim();
    if (cache[v].valid && cache[v].prefetched_unused)
        prefetch_note_outcome(cache[v].lba, 0);  /* prefetch evicted unused */
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
    bcache_restore(f);
}

/* Insert a block at the MRU end, marked prefetched. See bcache.h. */
void bcache_prefetch_insert_mru(uint32_t lba, const uint8_t *data)
{
    uint64_t f = bcache_cli_save();
    int idx = bcache_lookup(lba);
    if (idx >= 0) {
        bcache_touch(idx);
        bcache_restore(f);
        return;
    }

    bcache_restore(f);
    if (bcache_clean_victim() < 0)
        return;  /* write-back failed: skip this prefetch */
    f = bcache_cli_save();
    if (bcache_lookup(lba) >= 0) { bcache_restore(f); return; }
    int v = bcache_victim();
    if (cache[v].valid && cache[v].prefetched_unused)
        prefetch_note_outcome(cache[v].lba, 0);  /* prefetch evicted unused */
    cache[v].lba = lba;
    for (int i = 0; i < 512; i++)
        cache[v].data[i] = data[i];
    cache[v].valid = 1;
    cache[v].prefetched_unused = 1;
    cache[v].dirty = 0;
    bcache_touch(v);  /* MRU end: newest sequence number */
    bcache_restore(f);
}

void bcache_prefetch_mru(uint32_t lba)
{
    if (bcache_lookup(lba) >= 0)
        return;

    uint8_t tmp[512];
    if (ata_read(lba, 1, tmp) != 0)
        return;  /* best-effort: ignore errors */

    ata_lock();
    bcache_prefetch_insert_mru(lba, tmp);
    ata_unlock();
}

void bcache_invalidate(void)
{
    /* Dirty entries must reach disk before being dropped. */
    bcache_sync();
    uint64_t f = bcache_cli_save();
    for (int i = 0; i < BCACHE_SIZE; i++)
        cache[i].valid = 0;
    bcache_restore(f);
}

/* Cache-state probe (Phase 13 inc.8): bit0 valid, bit1 dirty,
 * bit2 prefetched-but-unused. Lets tests observe the cache
 * directly instead of inferring it from hit counts. */
int bcache_probe(uint32_t lba)
{
    uint64_t f = bcache_cli_save();
    int idx = bcache_lookup(lba);
    int bits = 0;
    if (idx >= 0) {
        bits = 1;
        if (cache[idx].dirty) bits |= 2;
        if (cache[idx].prefetched_unused) bits |= 4;
    }
    bcache_restore(f);
    return bits;
}

/* Invalidate every entry in [lba, lba+count) (Phase 13 inc.8).
 * Called when the filesystem frees a sector range (MyFS tail
 * rollback): after the free, no cached copy of the range may
 * survive to be served or written back over the record that
 * reuses the sectors.
 *
 * Dirty semantics: REFUSE. If any entry in the range is dirty
 * the function drops nothing and returns -1. A dirty entry
 * holds the only copy of unwritten data; dropping it would
 * lose that data, and writing it back from here would put
 * this primitive in the write-ordering business. The call
 * site (diskfs_rollback_tail) invokes it immediately after a
 * successful bcache_sync(), so the range is clean by
 * construction and the refusal cannot fire unless the write
 * path's sync discipline breaks — in which case failing
 * loudly is the point. Sector 0 is never a freed record
 * sector and is refused as a range start.
 *
 * Shadow tags for the range are dropped too (they hold no
 * data, but a stale tag would skew the pollution statistic
 * for the reused sector). Queued prefetch entries are NOT
 * removed: the worker reads the sector from disk when it
 * processes an entry (under the ATA mutex, atomically with
 * the insert, whose no-overwrite rule protects demand-written
 * entries), so a queued entry carries no bytes across the
 * free. Returns the number of cache entries dropped. */
int bcache_invalidate_range(uint32_t lba, uint32_t count)
{
    if (lba == 0) return -1;
    uint64_t f = bcache_cli_save();
    for (uint32_t k = 0; k < count; k++) {
        int idx = bcache_lookup(lba + k);
        if (idx >= 0 && cache[idx].dirty) {
            bcache_restore(f);
            return -1;
        }
    }
    int dropped = 0;
    for (uint32_t k = 0; k < count; k++) {
        int idx = bcache_lookup(lba + k);
        if (idx >= 0) {
            if (cache[idx].prefetched_unused)
                prefetch_note_outcome(lba + k, 0);
            cache[idx].valid = 0;
            dropped++;
        }
        for (int i = 0; i < BCACHE_SIZE; i++)
            if (shadow[i].valid && shadow[i].lba == lba + k)
                shadow[i].valid = 0;
    }
    bcache_restore(f);
    return dropped;
}

uint64_t bcache_hits(void)   { return stat_hits; }
uint64_t bcache_misses(void) { return stat_misses; }

/* -----------------------------------------------------------------------
 * Async prefetch queue + worker (Phase 4, increment 2).
 *
 * Policies no longer read the disk themselves: they enqueue candidate
 * LBAs here and return immediately, so a demand access never pays for
 * speculative reads inline (the Phase 3 timing eval showed synchronous
 * prefetching made the heuristic baselines 3.5-5x SLOWER than no
 * prefetching). A kernel worker task drains the queue: it sleeps while
 * the queue is empty, and its reads block on IRQ14 (inc.1) and
 * serialise with demand reads through the ATA mutex. While a demand
 * task is blocked on its own read, the worker is the task that runs,
 * so prefetches overlap the demand wait instead of adding to it.
 * ----------------------------------------------------------------------- */
#define PREFETCH_QUEUE_SIZE 128

struct pf_entry { uint32_t lba; uint8_t mru; uint64_t seq; };
static struct pf_entry pf_queue[PREFETCH_QUEUE_SIZE];
static uint32_t pf_head = 0;    /* index of oldest entry */
static uint32_t pf_count = 0;   /* number of queued entries */
static uint64_t stat_pf_queued  = 0;
static uint64_t stat_pf_dropped = 0;
static uint64_t stat_pf_done    = 0;
static struct task *pf_worker_task = 0;

void bcache_prefetch_enqueue(uint32_t lba, int mru)
{
    uint64_t f = bcache_cli_save();
    if (bcache_lookup(lba) >= 0) { bcache_restore(f); return; }
    for (uint32_t i = 0; i < pf_count; i++) {
        if (pf_queue[(pf_head + i) % PREFETCH_QUEUE_SIZE].lba == lba) {
            bcache_restore(f);
            return;   /* already queued */
        }
    }
    if (pf_count >= PREFETCH_QUEUE_SIZE) {
        stat_pf_dropped++;
        bcache_restore(f);
        return;
    }
    uint32_t slot = (pf_head + pf_count) % PREFETCH_QUEUE_SIZE;
    pf_queue[slot].lba = lba;
    pf_queue[slot].mru = mru ? 1 : 0;
    pf_queue[slot].seq = acc_seq;   /* demand-stream position at enqueue */
    pf_count++;
    stat_pf_queued++;
    struct task *w = pf_worker_task;
    bcache_restore(f);
    if (w) {
        sched_wake_io(w);
        sched_request_resched();
    }
}

void bcache_prefetch_stats(uint64_t *queued, uint64_t *dropped,
                           uint64_t *done)
{
    if (queued)  *queued  = stat_pf_queued;
    if (dropped) *dropped = stat_pf_dropped;
    if (done)    *done    = stat_pf_done;
}

/* Was lba demanded after access-sequence `seq`? Scans the demand
 * trace ring backwards from the newest record. The ring is written by
 * demand tasks without a lock; a torn read here is benign (the check
 * is a heuristic skip: worst case we drop a live entry or read a
 * stale one). */
static int pf_demanded_since(uint32_t lba, uint64_t seq)
{
    for (uint32_t i = 0; i < acc_count; i++) {
        uint32_t slot = (acc_head + acc_count - 1 - i) % ACCESS_TRACE_RING_SIZE;
        if (acc_ring[slot].seq <= seq)
            break;
        if (acc_ring[slot].lba == lba)
            return 1;
    }
    return 0;
}

/* Worker task body (created by kernel_main via task_create). */
void bcache_prefetch_worker(void)
{
    pf_worker_task = sched_current_task();
    uint8_t tmp[512];
    for (;;) {
        uint64_t f = bcache_cli_save();
        if (pf_count == 0) {
            bcache_restore(f);
            /* The 5-tick deadline is the lost-wake backstop: an
             * enqueue can land between the empty check above and the
             * state store inside sched_block_current, where its wake
             * would find us not yet BLOCKED and be dropped. */
            sched_block_current((uint64_t)timer_ticks() + 5);
            continue;
        }
        struct pf_entry e = pf_queue[pf_head];
        pf_head = (pf_head + 1) % PREFETCH_QUEUE_SIZE;
        pf_count--;
        bcache_restore(f);

        if (bcache_lookup(e.lba) >= 0)
            continue;   /* filled meanwhile */
        if (pf_demanded_since(e.lba, e.seq))
            continue;   /* too late: the demand for this block already
                         * went to disk. Near-frontier candidates (+1)
                         * are almost always lost this way on a single
                         * serialised disk (the demand task re-takes the
                         * lock before the worker can run); skipping them
                         * for free is what lets the worker spend its
                         * disk slots on the +2..+4 candidates that are
                         * still ahead of the frontier. */
        /* Take the disk mutex and RE-CHECK: while we waited for it,
         * the demanding task usually read this very block itself (its
         * read was in flight when the entry was queued). Without the
         * re-check the worker wastes its disk slots re-reading blocks
         * the demand stream just cached, stays permanently one step
         * behind the frontier, and no prefetch ever converts to a hit. */
        /* Hold the disk mutex across a small batch: the demand task
         * is queued on the mutex, and every extra block read in this
         * tenure is one its stream will not have to wait for. The
         * under-lock re-check stays essential (see read_ctx). */
        ata_lock();
        int budget = 4;
        for (;;) {
            if (bcache_lookup(e.lba) >= 0) {
                /* already cached: nothing to do */
            } else if (ata_read_locked(e.lba, 1, tmp) == 0) {
                if (e.mru)
                    bcache_prefetch_insert_mru(e.lba, tmp);
                else
                    bcache_prefetch_insert(e.lba, tmp);
                stat_pf_done++;
            } else {
                /* best-effort: ignore read errors */
            }
            if (--budget <= 0)
                break;
            uint64_t f2 = bcache_cli_save();
            if (pf_count == 0) { bcache_restore(f2); break; }
            e = pf_queue[pf_head];
            pf_head = (pf_head + 1) % PREFETCH_QUEUE_SIZE;
            pf_count--;
            bcache_restore(f2);
            /* The lookup at the top of the loop filters entries that
             * are already cached; an entry whose demand already went
             * to disk is necessarily cached too (see pf_demanded_since
             * for why the first entry needs the explicit check). */
        }
        ata_unlock();
    }
}
