#include "prefetch.h"
#include "bcache.h"

/* -----------------------------------------------------------------------
 * Prefetch policies (Phase 3, increment 4).
 *
 * none:       No prefetching. Baseline: measures reuse-only hit rate.
 * sequential: Linux-style. Detects sequential runs (lba == prev+1) and
 *             prefetches the next 4 blocks. Resets on non-sequential.
 * stride:     Detects constant stride (lba - prev == prev - prevprev) and
 *             prefetches the next 4 blocks along the stride.
 * ----------------------------------------------------------------------- */

static struct prefetch_policy *active_policy = &prefetch_none;

void prefetch_set_policy(struct prefetch_policy *p)
{
    if (p) {
        active_policy = p;
        p->reset();
    }
}

struct prefetch_policy *prefetch_get_policy(void)
{
    return active_policy;
}

void prefetch_on_access(uint32_t lba, int hit)
{
    if (active_policy && active_policy->on_access)
        active_policy->on_access(lba, hit);
}

/* --- none: no prefetching --- */
static void none_on_access(uint32_t lba, int hit)
{
    (void)lba; (void)hit;
}
static void none_reset(void) {}

struct prefetch_policy prefetch_none = {
    .name = "none",
    .on_access = none_on_access,
    .reset = none_reset,
};

/* --- sequential: prefetch next 4 on sequential run --- */
static uint32_t seq_last_lba = 0;
static int seq_has_last = 0;

static void seq_on_access(uint32_t lba, int hit)
{
    (void)hit;
    if (seq_has_last && lba == seq_last_lba + 1) {
        /* Sequential: prefetch next 4 blocks. */
        for (uint32_t i = 1; i <= 4; i++)
            bcache_prefetch(lba + i);
    }
    seq_last_lba = lba;
    seq_has_last = 1;
}
static void seq_reset(void)
{
    seq_has_last = 0;
}

struct prefetch_policy prefetch_sequential = {
    .name = "sequential",
    .on_access = seq_on_access,
    .reset = seq_reset,
};

/* --- stride: detect constant stride, prefetch along it --- */
static uint32_t stride_lba[3] = {0, 0, 0};
static int stride_count = 0;

static void stride_on_access(uint32_t lba, int hit)
{
    (void)hit;
    stride_lba[0] = stride_lba[1];
    stride_lba[1] = stride_lba[2];
    stride_lba[2] = lba;
    if (stride_count < 3) stride_count++;

    if (stride_count >= 3) {
        int32_t s1 = (int32_t)(stride_lba[1] - stride_lba[0]);
        int32_t s2 = (int32_t)(stride_lba[2] - stride_lba[1]);
        if (s1 == s2 && s1 != 0) {
            /* Constant stride detected: prefetch next 4 along stride. */
            for (int i = 1; i <= 4; i++)
                bcache_prefetch(lba + (uint32_t)(s2 * i));
        }
    }
}
static void stride_reset(void)
{
    stride_count = 0;
}

struct prefetch_policy prefetch_stride = {
    .name = "stride",
    .on_access = stride_on_access,
    .reset = stride_reset,
};
