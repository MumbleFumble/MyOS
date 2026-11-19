#include "prefetch.h"
#include "bcache.h"
#include "vfs.h"

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

void prefetch_on_access(uint32_t lba, int hit, uint32_t file_id,
                        uint32_t file_off)
{
    if (active_policy && active_policy->on_access)
        active_policy->on_access(lba, hit, file_id, file_off);
}

void prefetch_note_outcome(uint32_t lba, int useful)
{
    if (active_policy && active_policy->note_outcome)
        active_policy->note_outcome(lba, useful);
}

/* --- none: no prefetching --- */
static void none_on_access(uint32_t lba, int hit, uint32_t file_id,
                           uint32_t file_off)
{
    (void)lba; (void)hit; (void)file_id; (void)file_off;
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

static void seq_on_access(uint32_t lba, int hit, uint32_t file_id,
                          uint32_t file_off)
{
    (void)hit; (void)file_id; (void)file_off;
    if (seq_has_last && lba == seq_last_lba + 1) {
        /* Sequential: prefetch next 4 blocks. */
        for (uint32_t i = 1; i <= 4; i++)
            bcache_prefetch_enqueue(lba + i, 0);
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

static void stride_on_access(uint32_t lba, int hit, uint32_t file_id,
                             uint32_t file_off)
{
    (void)hit; (void)file_id; (void)file_off;
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
                bcache_prefetch_enqueue(lba + (uint32_t)(s2 * i), 0);
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

/* --- learned: candidate generation + linear scoring (inc.7, file
 * features inc.10; online updates Phase 10) ---
 *
 * Kernel port of scripts/sim_prefetch.py's LearnedPolicy, with weights
 * trained offline by scripts/train_prefetch.py (prefetch_weights.h,
 * fixed point x1024). On each access it scores candidate targets and
 * prefetches (MRU-end insertion) every candidate scoring above the
 * threshold. Candidates are the global offsets {1,2,3,4, d1, 2*d1, d2}
 * plus the next block of up to 3 other recently-read files.
 *
 * Accuracy feedback comes from bcache via note_outcome: a prefetched
 * block that is hit is useful, one evicted unused is wasted; the last
 * 32 outcomes form the recent_acc feature.
 *
 * File features (inc.10): diskfs supplies (file_id, file_off) with each
 * demand. A per-file table tracks the last two offsets per file, so the
 * policy can see each file's own stream (file_seq, file_stride) and
 * prefetch another file's continuation before the reader switches back
 * to it (file_seq_other, the single largest trained weight).
 *
 * Phase 10 (online learning): the weights live in a mutable array
 * lw[], seeded from the trained values. Scoring is vectorized
 * (score = sum(lw[i] * x[i]) / 1024), which is bit-identical to the
 * original per-term arithmetic for the frozen policy. The
 * learned-online policy (index 4) additionally updates lw[] at
 * runtime with a margin perceptron:
 *   - an issued prefetch's feature vector is remembered by LBA; when
 *     bcache reports its outcome, useful reinforces (if the score was
 *     not already confident) and wasted penalizes;
 *   - a rejected candidate's vector is also remembered; if the block
 *     is demanded within 1024 accesses (a false negative), its
 *     weights are reinforced.
 * Updates move each weight by x[i]/16, clamped to +-8192. Switching
 * policies reseeds lw[] from the trained weights.
 */
#include "prefetch_weights.h"

#define L_HIST 8
#define L_SEEN_SIZE 1024
#define L_FILES 16
#define L_NW 13
#define L_TAB 64

/* Weight indices. */
#define LW_BIAS          0
#define LW_IS_NEXT       1
#define LW_STRIDE_CONF   2
#define LW_STRIDE_UNCONF 3
#define LW_SEQ_RUN       4
#define LW_DIST          5
#define LW_RECENT_ACC    6
#define LW_REUSE         7
#define LW_JUMP          8
#define LW_STREAM_CONT   9
#define LW_FILE_SEQ      10
#define LW_FILE_SEQ_OTHER 11
#define LW_FILE_STRIDE   12

static const int32_t lw_seed[L_NW] = {
    PW_BIAS, PW_IS_NEXT, PW_STRIDE_CONFIRMED, PW_STRIDE_UNCONF,
    PW_SEQ_RUN, PW_DIST, PW_RECENT_ACC, PW_REUSE, PW_JUMP,
    PW_STREAM_CONT, PW_FILE_SEQ, PW_FILE_SEQ_OTHER, PW_FILE_STRIDE
};
static int32_t lw[L_NW] = {
    PW_BIAS, PW_IS_NEXT, PW_STRIDE_CONFIRMED, PW_STRIDE_UNCONF,
    PW_SEQ_RUN, PW_DIST, PW_RECENT_ACC, PW_REUSE, PW_JUMP,
    PW_STREAM_CONT, PW_FILE_SEQ, PW_FILE_SEQ_OTHER, PW_FILE_STRIDE
};
static int l_online = 0;
static uint32_t l_upd_pos = 0, l_upd_neg = 0;

/* Phase 10 inc.3: persisted "home" weights (defined at the bottom of
 * this file). The online policy seeds from lw_home, which l_load_home
 * fills from pfweights.bin when it exists; otherwise the trained
 * values. l_loading guards on_access against the load's own VFS
 * reads. */
#define PFW_MAGIC 0x50574631u   /* "PWF1" */
static int32_t lw_home[L_NW] = {
    PW_BIAS, PW_IS_NEXT, PW_STRIDE_CONFIRMED, PW_STRIDE_UNCONF,
    PW_SEQ_RUN, PW_DIST, PW_RECENT_ACC, PW_REUSE, PW_JUMP,
    PW_STREAM_CONT, PW_FILE_SEQ, PW_FILE_SEQ_OTHER, PW_FILE_STRIDE
};
static int l_home_tried = 0;
static int l_loading = 0;

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

/* Per-file stream state (inc.10). lf_id 0 = empty slot. */
static uint32_t lf_id[L_FILES];
static uint32_t lf_last_off[L_FILES];
static uint32_t lf_last_lba[L_FILES];
static uint32_t lf_prev_off[L_FILES];
static int lf_has_prev[L_FILES];
static uint32_t lf_order[L_FILES];  /* file_ids, most recent last */
static int lf_order_n;

/* Online credit assignment: feature vectors of issued prefetches
 * (joined by LBA when the outcome arrives) and of rejected
 * candidates (joined if the block is demanded soon after). */
struct l_rec {
    uint32_t lba;
    int32_t score;
    int32_t x[L_NW];
};
static struct l_rec l_issued[L_TAB];
static struct l_rec l_rej[L_TAB];
static uint64_t l_rej_idx[L_TAB];
static int l_issued_pos, l_rej_pos;
static int l_tab_ready = 0;

static void l_tab_init(void)
{
    for (int i = 0; i < L_TAB; i++) {
        l_issued[i].lba = 0xFFFFFFFFu;
        l_rej[i].lba = 0xFFFFFFFFu;
    }
    l_issued_pos = 0;
    l_rej_pos = 0;
    l_tab_ready = 1;
}

static int lf_find(uint32_t file_id)
{
    for (int i = 0; i < L_FILES; i++)
        if (lf_id[i] == file_id)
            return i;
    return -1;
}

static int32_t l_score(const int32_t *x)
{
    int32_t s = 0;
    for (int i = 0; i < L_NW; i++)
        s += (lw[i] * x[i]) / 1024;
    return s;
}

static void l_apply_update(const int32_t *x, int32_t score, int positive)
{
    if (positive && score >= 1024)
        return;   /* already confident and right */
    if (!positive && score <= -1024)
        return;   /* already confident and wrong (cannot happen for issued) */
    for (int i = 0; i < L_NW; i++) {
        int32_t d = x[i] >> 4;   /* learning rate: 1/16 of the feature */
        lw[i] = positive ? lw[i] + d : lw[i] - d;
        if (lw[i] > 8192) lw[i] = 8192;
        if (lw[i] < -8192) lw[i] = -8192;
    }
    if (positive)
        l_upd_pos++;
    else
        l_upd_neg++;
}

static void l_record(struct l_rec *tab, int *pos, uint32_t lba,
                     const int32_t *x, int32_t score)
{
    struct l_rec *r = &tab[*pos];
    r->lba = lba;
    r->score = score;
    for (int i = 0; i < L_NW; i++)
        r->x[i] = x[i];
    *pos = (*pos + 1) % L_TAB;
}

static void l_check_false_negative(uint32_t lba)
{
    for (int i = 0; i < L_TAB; i++) {
        if (l_rej[i].lba == lba
            && l_index - l_rej_idx[i] <= 1024) {
            l_apply_update(l_rej[i].x, l_rej[i].score, 1);
            l_rej[i].lba = 0xFFFFFFFFu;
            return;
        }
    }
}

static void learned_note_outcome(uint32_t lba, int useful)
{
    if (l_out_n == 32 && (l_out_mask & 0x80000000u))
        l_out_useful--;
    l_out_mask = (l_out_mask << 1) | (useful ? 1u : 0u);
    if (useful)
        l_out_useful++;
    if (l_out_n < 32)
        l_out_n++;
    if (l_online) {
        if (!l_tab_ready)
            l_tab_init();
        for (int i = 0; i < L_TAB; i++) {
            if (l_issued[i].lba == lba) {
                l_apply_update(l_issued[i].x, l_issued[i].score, useful);
                l_issued[i].lba = 0xFFFFFFFFu;
                break;
            }
        }
    }
}

static void learned_reset_common(void)
{
    l_has1 = 0;
    l_has2 = 0;
    l_hist_len = 0;
    l_seq_run = 0;
    l_out_mask = 0;
    l_out_n = 0;
    l_out_useful = 0;
    l_index = 0;
    l_upd_pos = 0;
    l_upd_neg = 0;
    for (int i = 0; i < L_SEEN_SIZE; i++) {
        l_seen_lba[i] = 0xFFFFFFFFu;
        l_seen_idx[i] = 0;
    }
    for (int i = 0; i < L_FILES; i++) {
        lf_id[i] = 0;
        lf_has_prev[i] = 0;
    }
    lf_order_n = 0;
    l_tab_init();
}

static void learned_reset(void)
{
    learned_reset_common();
    l_online = 0;
    for (int i = 0; i < L_NW; i++)
        lw[i] = lw_seed[i];
}

static void l_load_home(void);

static void learned_online_reset(void)
{
    learned_reset_common();
    l_online = 1;
    l_load_home();
    for (int i = 0; i < L_NW; i++)
        lw[i] = lw_home[i];
}

/* Fixed-point feature value for distance: 1024 / (1 + floor(log2(a))). */
static int32_t l_dist_fp(uint32_t a)
{
    int lg = 0;
    while ((a >> (lg + 1)) != 0)
        lg++;
    return 1024 / (1 + lg);
}

static void learned_on_access(uint32_t lba, int hit, uint32_t file_id,
                              uint32_t file_off)
{
    (void)hit;
    if (l_loading)
        return;   /* VFS reads from the weight load must not recurse */
    l_index++;
    if (l_online) {
        if (!l_tab_ready)
            l_tab_init();
        l_check_false_negative(lba);
    }
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

    /* Current file's pre-access stream state. */
    int cur = (file_id != 0) ? lf_find(file_id) : -1;

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
        int32_t x[L_NW];
        for (int i = 0; i < L_NW; i++)
            x[i] = 0;
        x[LW_BIAS] = 1024;
        if (off == 1)
            x[LW_IS_NEXT] = 1024;
        if (have_d1 && d1 != 0 && off == d1 && d1 == d2)
            x[LW_STRIDE_CONF] = 1024;
        if (have_d1 && d1 != 0 && off == d1 && d1 != d2)
            x[LW_STRIDE_UNCONF] = 1024;
        if (off >= 1 && off <= 4) {
            uint32_t run = l_seq_run < 4 ? l_seq_run : 4;
            x[LW_SEQ_RUN] = (int32_t)run * 256;
        }
        x[LW_DIST] = l_dist_fp((uint32_t)off);
        x[LW_RECENT_ACC] = acc_fp;
        uint32_t ts = target % L_SEEN_SIZE;
        if (l_seen_lba[ts] == target && l_index - l_seen_idx[ts] <= 512)
            x[LW_REUSE] = 1024;
        if (have_d1 && (d1 > 4 || d1 < -4))
            x[LW_JUMP] = 1024;
        if (off == 1 && stream_cont)
            x[LW_STREAM_CONT] = 1024;
        if (cur >= 0) {
            if (file_off == lf_last_off[cur] + 1 && off == 1)
                x[LW_FILE_SEQ] = 1024;
            if (lf_has_prev[cur]) {
                int32_t fd_now = (int32_t)(file_off - lf_last_off[cur]);
                int32_t fd_prev = (int32_t)(lf_last_off[cur] - lf_prev_off[cur]);
                if (fd_now != 0 && fd_now == fd_prev && off == fd_now)
                    x[LW_FILE_STRIDE] = 1024;
            }
        }

        int32_t score = l_score(x);
        if (score > PW_THRESHOLD) {
            bcache_prefetch_enqueue(target, 1);
            if (l_online)
                l_record(l_issued, &l_issued_pos, target, x, score);
        } else if (l_online) {
            l_record(l_rej, &l_rej_pos, target, x, score);
            l_rej_idx[(l_rej_pos + L_TAB - 1) % L_TAB] = l_index;
        }
    }

    /* Other files' continuations (file_seq_other). */
    if (file_id != 0) {
        int others = 0;
        for (int oi = lf_order_n - 1; oi >= 0 && others < 3; oi--) {
            uint32_t fid = lf_order[oi];
            if (fid == file_id)
                continue;
            int fi = lf_find(fid);
            if (fi < 0)
                continue;
            uint32_t target = lf_last_lba[fi] + 1;
            uint32_t gap = target > lba ? target - lba : lba - target;
            int32_t x[L_NW];
            for (int i = 0; i < L_NW; i++)
                x[i] = 0;
            x[LW_BIAS] = 1024;
            x[LW_DIST] = l_dist_fp(gap);
            x[LW_RECENT_ACC] = acc_fp;
            x[LW_FILE_SEQ_OTHER] = 1024;
            int32_t score = l_score(x);
            if (score > PW_THRESHOLD) {
                bcache_prefetch_enqueue(target, 1);
                if (l_online)
                    l_record(l_issued, &l_issued_pos, target, x, score);
            } else if (l_online) {
                l_record(l_rej, &l_rej_pos, target, x, score);
                l_rej_idx[(l_rej_pos + L_TAB - 1) % L_TAB] = l_index;
            }
            others++;
        }
    }

    /* Update per-file state after scoring. */
    if (file_id != 0) {
        int fi = cur;
        if (fi < 0) {
            /* Claim a free slot, else evict the least recently read
             * file (and drop its id from the order list). */
            fi = lf_find(0);
            if (fi < 0) {
                for (int oi = 0; oi < lf_order_n && fi < 0; oi++) {
                    int cand = lf_find(lf_order[oi]);
                    if (cand >= 0) {
                        fi = cand;
                        for (int i = oi; i + 1 < lf_order_n; i++)
                            lf_order[i] = lf_order[i + 1];
                        lf_order_n--;
                    }
                }
            }
            if (fi < 0)
                fi = 0;  /* unreachable: order covers every live slot */
            lf_id[fi] = file_id;
            lf_has_prev[fi] = 0;
            lf_last_off[fi] = file_off;
            lf_last_lba[fi] = lba;
        } else {
            lf_prev_off[fi] = lf_last_off[fi];
            lf_has_prev[fi] = 1;
            lf_last_off[fi] = file_off;
            lf_last_lba[fi] = lba;
        }
        /* Move file_id to the most-recent end of lf_order. */
        int pos = -1;
        for (int i = 0; i < lf_order_n; i++)
            if (lf_order[i] == file_id)
                pos = i;
        if (pos >= 0) {
            for (int i = pos; i + 1 < lf_order_n; i++)
                lf_order[i] = lf_order[i + 1];
            lf_order_n--;
        } else if (lf_order_n == L_FILES) {
            for (int i = 0; i + 1 < L_FILES; i++)
                lf_order[i] = lf_order[i + 1];
            lf_order_n = L_FILES - 1;
        }
        lf_order[lf_order_n++] = file_id;
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

struct prefetch_policy prefetch_learned_online = {
    .name = "learned-online",
    .on_access = learned_on_access,
    .reset = learned_online_reset,
    .note_outcome = learned_note_outcome,
};

/* Runtime weight access for SYS_PFWEIGHTS (Phase 10). */
void prefetch_weights_read(int32_t out[L_NW], uint32_t *pos, uint32_t *neg)
{
    for (int i = 0; i < L_NW; i++)
        out[i] = lw[i];
    *pos = l_upd_pos;
    *neg = l_upd_neg;
}

void prefetch_weights_zero(void)
{
    for (int i = 0; i < L_NW; i++)
        lw[i] = 0;
    l_upd_pos = 0;
    l_upd_neg = 0;
}

void prefetch_weights_seed(void)
{
    if (l_online) {
        l_load_home();
        for (int i = 0; i < L_NW; i++)
            lw[i] = lw_home[i];
    } else {
        for (int i = 0; i < L_NW; i++)
            lw[i] = lw_seed[i];
    }
}

/* Phase 10 inc.3: weight persistence. pfweights.bin on the MyFS disk
 * holds {magic, 13 x int32 LE}. The online policy seeds from it (via
 * lw_home) so learning survives reboots; the frozen policy always
 * seeds from the trained weights and is unaffected. */
static void l_load_home(void)
{
    if (l_home_tried)
        return;
    l_home_tried = 1;
    l_loading = 1;
    vfs_node_t *n = vfs_open("pfweights.bin");
    if (n) {
        uint8_t buf[4 + 4 * L_NW];
        if (vfs_read(n, 0, sizeof(buf), buf) == (int64_t)sizeof(buf)) {
            uint32_t magic = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8)
                           | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
            if (magic == PFW_MAGIC) {
                for (int i = 0; i < L_NW; i++) {
                    const uint8_t *b = &buf[4 + 4 * i];
                    int32_t w = (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8)
                                | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
                    if (w > 8192) w = 8192;
                    if (w < -8192) w = -8192;
                    lw_home[i] = w;
                }
            }
        }
        vfs_close(n);
    }
    l_loading = 0;
}

int prefetch_weights_save(void)
{
    uint8_t buf[4 + 4 * L_NW];
    buf[0] = (uint8_t)(PFW_MAGIC & 0xFF);
    buf[1] = (uint8_t)((PFW_MAGIC >> 8) & 0xFF);
    buf[2] = (uint8_t)((PFW_MAGIC >> 16) & 0xFF);
    buf[3] = (uint8_t)((PFW_MAGIC >> 24) & 0xFF);
    for (int i = 0; i < L_NW; i++) {
        uint32_t w = (uint32_t)lw[i];
        buf[4 + 4 * i + 0] = (uint8_t)(w & 0xFF);
        buf[4 + 4 * i + 1] = (uint8_t)((w >> 8) & 0xFF);
        buf[4 + 4 * i + 2] = (uint8_t)((w >> 16) & 0xFF);
        buf[4 + 4 * i + 3] = (uint8_t)((w >> 24) & 0xFF);
    }
    vfs_node_t *n = vfs_create("pfweights.bin");
    if (!n)
        return -1;
    int64_t put = vfs_write(n, 0, sizeof(buf), buf);
    vfs_close(n);
    if (put != (int64_t)sizeof(buf))
        return -1;
    bcache_sync();
    for (int i = 0; i < L_NW; i++)
        lw_home[i] = lw[i];
    l_home_tried = 1;
    return 0;
}
