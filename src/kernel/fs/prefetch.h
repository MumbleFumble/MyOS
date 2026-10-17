#pragma once
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Prefetch policy interface (Phase 3, increment 4).
 *
 * Policies observe cache accesses and issue prefetches. Called on every
 * bcache_read (hit or miss). The policy may call bcache_prefetch() to
 * speculatively load blocks.
 *
 * Baselines: none, sequential, stride. Oracle (perfect knowledge) is
 * evaluated offline in the simulator.
 * ----------------------------------------------------------------------- */

struct prefetch_policy {
    const char *name;
    /* Called on each cache access. lba = requested block, hit = 1 if cache hit. */
    void (*on_access)(uint32_t lba, int hit);
    /* Reset policy state (on policy switch). */
    void (*reset)(void);
};

/* Available policies. */
extern struct prefetch_policy prefetch_none;
extern struct prefetch_policy prefetch_sequential;
extern struct prefetch_policy prefetch_stride;

/* Active policy management. */
void prefetch_set_policy(struct prefetch_policy *p);
struct prefetch_policy *prefetch_get_policy(void);
void prefetch_on_access(uint32_t lba, int hit);
