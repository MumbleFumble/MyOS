#pragma once

#include "sched.h"

/*
 * Learned scheduling policy (policy index 2).
 *
 * A linear scoring policy whose weights were learned offline via a
 * trace-calibrated simulator (scripts/sim_search.py). For each READY
 * candidate the policy computes:
 *
 *   score = MLW_TOTAL  * total_ticks
 *         + MLW_SYS    * syscall_count
 *         + MLW_IDLE   * (pid == 1)
 *         + MLW_OTHERS * (sum of total_ticks of other READY tasks)
 *
 * ...in 2^20 fixed point (integer-only, no FP in the kernel), and picks
 * the candidate with the minimum score. The learned weights:
 *   - strongly avoid the idle task (pid 1),
 *   - prefer tasks with high syscall counts (I/O-bound / interactive),
 *   - prefer tasks with low accumulated run time (fairness; no starvation).
 *
 * Weights live in policy_ml_weights.h (auto-generated, do not edit by hand).
 */
extern struct sched_policy ml_policy;
