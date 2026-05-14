#include "policy_wf.h"
#include "sched.h"

/*
 * Weighted-fair pick_next.
 *
 * Score for each READY task (higher = more deserving):
 *
 *   score = wait_ticks * 1024 / max(total_ticks, 1)
 *
 * Integer-only arithmetic — no floating point in the kernel.
 * A task that has never run (total_ticks == 0) gets score = wait_ticks * 1024,
 * which is always >= any other score, so new tasks run promptly.
 */
static uint32_t wf_pick_next(uint32_t cur,
                              const struct task *tasks,
                              uint32_t count,
                              uint64_t tick)
{
    (void)tick;

    uint32_t best_idx   = cur;
    uint64_t best_score = 0;
    int      found      = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (tasks[i].state != TASK_READY)
            continue;

        uint64_t denom = tasks[i].total_ticks ? tasks[i].total_ticks : 1;
        uint64_t score = (tasks[i].wait_ticks * 1024) / denom;

        /* Bias: tasks that have never been scheduled get a floor score so
         * they aren't starved when an old task always has a higher wait ratio. */
        if (tasks[i].total_ticks == 0)
            score += 2048;

        if (!found || score > best_score) {
            best_score = score;
            best_idx   = i;
            found      = 1;
        }
    }

    return best_idx;
}

struct sched_policy wf_policy = {
    .name         = "weighted-fair",
    .init         = 0,
    .pick_next    = wf_pick_next,
    .task_added   = 0,
    .task_removed = 0,
};
