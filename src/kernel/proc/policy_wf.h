#pragma once

#include "sched.h"

/*
 * Weighted-fair scheduling policy (policy index 1).
 *
 * Assigns each task a score based on its observed behaviour:
 *
 *   cpu_ratio  = total_ticks / max(total_ticks + wait_ticks, 1)
 *
 * A task that has spent most of its life running (cpu_ratio → 1) is
 * CPU-heavy and gets a LOW priority score — it has already used a lot.
 * A task that has spent most of its life waiting (cpu_ratio → 0) is
 * I/O-bound or interactive and gets a HIGH priority score — it deserves
 * CPU time now.
 *
 * Among all READY tasks the one with the highest score is picked.
 * Ties are broken by index (stable relative order).
 */
extern struct sched_policy wf_policy;
