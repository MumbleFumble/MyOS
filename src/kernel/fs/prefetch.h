#pragma once
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Prefetch policy interface (Phase 3, increment 4).
 *
 * Policies observe cache accesses and issue prefetches. Called on every
 * bcache_read (hit or miss). Since Phase 4 inc.2 the policy does not
 * read the disk itself: it calls bcache_prefetch_enqueue() and a kernel
 * worker task loads the block asynchronously.
 *
 * Baselines: none, sequential, stride. Oracle (perfect knowledge) is
 * evaluated offline in the simulator.
 * ----------------------------------------------------------------------- */

struct prefetch_policy {
    const char *name;
    /* Called on each cache access. lba = requested block, hit = 1 if
     * cache hit. file_id/file_off give the demand's file context
     * (Phase 3 inc.9; file_id 0 = no file context). */
    void (*on_access)(uint32_t lba, int hit, uint32_t file_id,
                      uint32_t file_off);
    /* Reset policy state (on policy switch). */
    void (*reset)(void);
    /* Optional: told when a prefetched block is consumed (useful = 1) or
     * evicted unused (useful = 0), with the block's LBA for credit
     * assignment. Used by the learned policy's accuracy feedback and
     * (Phase 10) its online weight updates; NULL for the baselines. */
    void (*note_outcome)(uint32_t lba, int useful);
};

/* Available policies. */
extern struct prefetch_policy prefetch_none;
extern struct prefetch_policy prefetch_sequential;
extern struct prefetch_policy prefetch_stride;
extern struct prefetch_policy prefetch_learned;
extern struct prefetch_policy prefetch_learned_online;

/* Active policy management. */
void prefetch_set_policy(struct prefetch_policy *p);
struct prefetch_policy *prefetch_get_policy(void);
void prefetch_note_outcome(uint32_t lba, int useful);
void prefetch_on_access(uint32_t lba, int hit, uint32_t file_id,
                        uint32_t file_off);

/* Phase 10: runtime access to the learned weights (SYS_PFWEIGHTS). */
#define PF_NWEIGHTS 13
void prefetch_weights_read(int32_t out[PF_NWEIGHTS], uint32_t *pos,
                           uint32_t *neg);
void prefetch_weights_zero(void);
void prefetch_weights_seed(void);

