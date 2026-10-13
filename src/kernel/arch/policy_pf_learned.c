#include <stdint.h>
#include "exceptions.h"

/* -----------------------------------------------------------------------
 * Learned page replacement policy (Phase 2, increment 5).
 *
 * Linear model trained by scripts/sim_pf_search.py on the bench_pf
 * access pattern (236 accesses, 4-frame cache):
 *
 *     score = W_AGE * age + W_FREQ * freq   (fixed-point 2^20)
 *
 *   age  = faults since the frame was allocated (residency age)
 *   freq = total faults for the resident page (hotness)
 *
 * The frame with the HIGHEST score is evicted. Both weights are
 * negative: the policy keeps frequently-faulted pages and, among
 * similar frequencies, prefers evicting young pages (MRU-like
 * behavior that handles sequential scans well).
 *
 * Training results: 104 faults vs FIFO 188 (-45%), OPT 88.
 * Held-out pattern: 100 vs FIFO 179 (-44%), OPT 83.
 * ----------------------------------------------------------------------- */

#define FP_SHIFT 20
#define W_AGE_FP   (-1286698)   /* -1.227091 * 2^20 */
#define W_FREQ_FP  (-202484)    /* -0.193105 * 2^20 */

static int learned_pick_victim(struct demand_frame *frames, int n)
{
    uint64_t now = pf_current_seq();
    int victim = -1;
    int64_t best_score = 0;

    for (int i = 0; i < n; i++) {
        if (frames[i].virt == 0)
            continue;
        uint64_t age = now - frames[i].seq;
        uint64_t freq = frames[i].freq;
        /* Fixed-point: score = W_AGE*age + W_FREQ*freq. */
        int64_t score = (int64_t)W_AGE_FP * (int64_t)age +
                        (int64_t)W_FREQ_FP * (int64_t)freq;
        if (victim < 0 || score > best_score) {
            victim = i;
            best_score = score;
        }
    }
    return victim;
}

struct pf_policy pf_policy_learned = {
    .name = "learned",
    .pick_victim = learned_pick_victim,
};
