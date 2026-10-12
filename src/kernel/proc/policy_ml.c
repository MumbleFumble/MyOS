#include "policy_ml.h"
#include "policy_ml_weights.h"
#include "sched.h"

/*
 * Learned linear policy pick_next.
 *
 * Scores every TASK_READY candidate with the offline-learned weights
 * (fixed point, integer-only) and returns the index of the minimum.
 * Ties go to the lowest index (stable). If nothing is READY, returns
 * current_idx per the policy contract.
 *
 * Score features, all taken from the task snapshots the scheduler
 * already maintains:
 *   total_c      - total_ticks of the candidate
 *   syscalls_c   - syscall_count of the candidate
 *   is_idle_c    - 1 if the candidate is the idle task (pid == 1)
 *   total_others - sum of total_ticks over the other READY tasks
 */
static uint32_t ml_pick_next(uint32_t cur,
                             const struct task *tasks,
                             uint32_t count,
                             uint64_t tick)
{
    (void)tick;

    /* Sum of total_ticks over all runnable tasks (READY or RUNNING).
     * total_others for a candidate is this minus the candidate's own
     * total. Matches the simulator's feature definition. */
    uint64_t runnable_total_sum = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (tasks[i].state == TASK_READY || tasks[i].state == TASK_RUNNING)
            runnable_total_sum += tasks[i].total_ticks;
    }

    /* Score a task index using the learned weights. */
    /* (inlined below for cur and for each READY candidate) */

    uint32_t best_idx   = cur;
    int64_t  best_score = 0;
    int      found      = 0;

    /* Consider keeping the current task: if it is still RUNNING, score
     * it as a candidate. Returning cur makes sched_tick skip the switch
     * entirely (next == old), avoiding pointless idle<->work bouncing. */
    if (cur < count && tasks[cur].state == TASK_RUNNING) {
        uint64_t total_c      = tasks[cur].total_ticks;
        uint64_t syscalls_c   = tasks[cur].syscall_count;
        uint64_t is_idle_c    = (tasks[cur].pid == 1) ? 1 : 0;
        uint64_t total_others = runnable_total_sum - total_c;
        /* Note: ready_total_sum excludes the RUNNING task (it sums only
         * READY), so total_others for cur is the full sum. */
        best_score = MLW_TOTAL  * (int64_t)total_c
                   + MLW_SYS    * (int64_t)syscalls_c
                   + MLW_IDLE   * (int64_t)is_idle_c
                   + MLW_OTHERS * (int64_t)total_others;
        best_idx = cur;
        found    = 1;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (tasks[i].state != TASK_READY)
            continue;

        uint64_t total_c    = tasks[i].total_ticks;
        uint64_t syscalls_c = tasks[i].syscall_count;
        uint64_t is_idle_c  = (tasks[i].pid == 1) ? 1 : 0;
        uint64_t total_others = runnable_total_sum - total_c;

        /* Fixed-point weighted sum. The 2^20 scale is order-preserving
         * for argmin, so no shift is needed before comparing. All terms
         * fit comfortably in int64 for realistic tick/syscall counts. */
        int64_t score = MLW_TOTAL  * (int64_t)total_c
                      + MLW_SYS    * (int64_t)syscalls_c
                      + MLW_IDLE   * (int64_t)is_idle_c
                      + MLW_OTHERS * (int64_t)total_others;

        if (!found || score < best_score) {
            best_score = score;
            best_idx   = i;
            found      = 1;
        }
    }

    return best_idx;
}

struct sched_policy ml_policy = {
    .name         = "learned",
    .init         = 0,
    .pick_next    = ml_pick_next,
    .task_added   = 0,
    .task_removed = 0,
};
