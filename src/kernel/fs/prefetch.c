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

void prefetch_note_outcome(int useful)
{
    if (active_policy && active_policy->note_outcome)
        active_policy->note_outcome(useful);
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

/* --- learned: candidate generation + linear scoring (inc.7) ---
 *
 * Kernel port of scripts/sim_prefetch.py's LearnedPolicy, with weights
 * trained offline by scripts/train_prefetch.py (prefetch_weights.h,
 * fixed point x1024). On each access it scores candidate offsets
 * {1,2,3,4, d1, 2*d1, d2} over features of the recent stream and
 * prefetches (MRU-end insertion) every candidate scoring above the
 * threshold. Accuracy feedback comes from bcache via note_outcome:
 * a prefetched block that is hit is useful, one evicted unused is
 * wasted; the last 32 outcomes form the recent_acc feature.
 */
#include "prefetch_weights.h"

#define L_HIST 8
#define L_SEEN_SIZE 1024

static uint32_t l_prev1, l_prev2;
static int l_has1, l_has2;
static uint32_t l_hist[L_HIST];     /* previous accesses, oldest first */
static int l_hist_len;
static uint32_t l_seq_run;
static uint32_t l_out_mask;         /* last 32 outcomes, 1 = useful */
static int l_out_n, l_out_useful;
static uint64_t l_index;
static uint32_t l_seen_lba[L_SEEN_SIZE];
static uint32_t l_seen_idx[L_SEEN_SIZE];

static void learned_note_outcome(int useful)
{
    if (l_out_n == 32 && (l_out_mask & 0x80000000u))
        l_out_useful--;
    l_out_mask = (l_out_mask << 1) | (useful ? 1u : 0u);
    if (useful)
        l_out_useful++;
    if (l_out_n < 32)
        l_out_n++;
}

static void learned_reset(void)
{
    l_has1 = 0;
    l_has2 = 0;
    l_hist_len = 0;
    l_seq_run = 0;
    l_out_mask = 0;
    l_out_n = 0;
    l_out_useful = 0;
    l_index = 0;
    for (int i = 0; i < L_SEEN_SIZE; i++) {
        l_seen_lba[i] = 0xFFFFFFFFu;
        l_seen_idx[i] = 0;
    }
}

/* Fixed-point feature value for distance: 1024 / (1 + floor(log2(a))). */
static int32_t l_dist_fp(uint32_t a)
{
    int lg = 0;
    while ((a >> (lg + 1)) != 0)
        lg++;
    return 1024 / (1 + lg);
}

static void learned_on_access(uint32_t lba, int hit)
{
    (void)hit;
    l_index++;
    uint32_t slot = lba % L_SEEN_SIZE;
    l_seen_lba[slot] = lba;
    l_seen_idx[slot] = (uint32_t)l_index;

    int have_d1 = l_has1;
    int32_t d1 = have_d1 ? (int32_t)(lba - l_prev1) : 0;
    int32_t d2 = l_has2 ? (int32_t)(l_prev1 - l_prev2) : 0;

    if (have_d1 && d1 == 1)
        l_seq_run++;
    else
        l_seq_run = 0;

    /* recent accuracy, fixed point (512 = 0.5 cold start) */
    int32_t acc_fp = l_out_n ? (int32_t)((l_out_useful * 1024) / l_out_n) : 512;

    /* stream continuation: current lba is one past an access 2..8 back */
    int stream_cont = 0;
    for (int i = 0; i + 1 < l_hist_len; i++) {
        if ((int32_t)(lba - l_hist[i]) == 1) {
            stream_cont = 1;
            break;
        }
    }

    int32_t cands[7];
    cands[0] = 1; cands[1] = 2; cands[2] = 3; cands[3] = 4;
    cands[4] = d1; cands[5] = 2 * d1; cands[6] = d2;

    for (int ci = 0; ci < 7; ci++) {
        int32_t off = cands[ci];
        if (off <= 0 || off > 64)
            continue;
        int dup = 0;
        for (int cj = 0; cj < ci; cj++)
            if (cands[cj] == off)
                dup = 1;
        if (dup)
            continue;

        uint32_t target = lba + (uint32_t)off;
        int32_t score = PW_BIAS;
        if (off == 1)
            score += PW_IS_NEXT;
        if (have_d1 && d1 != 0 && off == d1 && d1 == d2)
            score += PW_STRIDE_CONFIRMED;
        if (have_d1 && d1 != 0 && off == d1 && d1 != d2)
            score += PW_STRIDE_UNCONF;
        if (off >= 1 && off <= 4) {
            uint32_t run = l_seq_run < 4 ? l_seq_run : 4;
            score += (PW_SEQ_RUN * (int32_t)run) / 4;
        }
        score += (PW_DIST * l_dist_fp((uint32_t)off)) / 1024;
        score += (PW_RECENT_ACC * acc_fp) / 1024;
        uint32_t ts = target % L_SEEN_SIZE;
        if (l_seen_lba[ts] == target && l_index - l_seen_idx[ts] <= 512)
            score += PW_REUSE;
        if (have_d1 && (d1 > 4 || d1 < -4))
            score += PW_JUMP;
        if (off == 1 && stream_cont)
            score += PW_STREAM_CONT;

        if (score > PW_THRESHOLD)
            bcache_prefetch_mru(target);
    }

    /* Push current access into history. */
    if (l_hist_len == L_HIST) {
        for (int i = 0; i + 1 < L_HIST; i++)
            l_hist[i] = l_hist[i + 1];
        l_hist_len = L_HIST - 1;
    }
    l_hist[l_hist_len++] = lba;
    l_prev2 = l_prev1;
    l_has2 = l_has1;
    l_prev1 = lba;
    l_has1 = 1;
}

struct prefetch_policy prefetch_learned = {
    .name = "learned",
    .on_access = learned_on_access,
    .reset = learned_reset,
    .note_outcome = learned_note_outcome,
};
