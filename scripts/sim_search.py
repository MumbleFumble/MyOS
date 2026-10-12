#!/usr/bin/env python3
"""Learn scheduling-policy weights via a trace-calibrated simulator.

Why a simulator (not fitted-Q regression):
  A single scheduling decision here shifts phase by ~1 tick, so true Q-value
  differences (~1) drown in return noise (~8). Regression then learns spurious
  time trends instead of choice effects. The simulator models the true
  dynamics -- calibrated to trace measurements -- so we can directly optimize
  the policy for the real objective: minimizing total scheduling delay.

Workload model (from traces / kernel telemetry, recalibrated Oct 2026):
  - Each benchmark needs B ticks of CPU to finish.
  - During a benchmark the runnable set is {idle, benchmark}; the shell
    blocks in sys_wait_pid. Neither idle nor benchmark blocks.
  - Picking the benchmark advances it by 1 tick; picking idle wastes the tick.
  - Cost per decision = (runnable - 1). Total cost = #decisions.

Scenarios (all must do well -- this prevents overfitting):
  - bench_cpu / bench_io / bench_mix: 2-task throughput workloads.
  - arrival: idle + taskA (50 ticks) + taskB (10 ticks, arrives at t=30).
    A policy that starves newcomers (e.g. "prefer high total_ticks")
    blows up here, so the search is forced to learn fairness too.

Policy: linear scorer, pick the runnable candidate with min score.
  Features (all integer-computable in the kernel):
    total_c      total_ticks of the candidate
    syscalls_c   syscall_count of the candidate
    is_idle_c    1 if candidate pid == 1 else 0  (idle is always pid 1)
    total_others sum of total_ticks of the other runnable candidates
  The bias is choice-independent so it never affects argmin; not searched.

Search: random search + hill-climbing, minimizing mean normalized cost.
  RR scores 2.0 on the throughput workloads; optimal is 1.0.
"""
import numpy as np

# (name, kind, params)
WORKLOADS = [
    ("bench_cpu", "throughput", {"B": 11, "syscalls": 12}),
    ("bench_io", "throughput", {"B": 242, "syscalls": 2602}),
    ("bench_mix", "throughput", {"B": 257, "syscalls": 2604}),
    ("arrival", "arrival", {"B_a": 50, "B_b": 10, "t_arrive": 30}),
]

FEATS = ["total_c", "syscalls_c", "is_idle_c", "total_others"]


def score_candidates(weights, tasks):
    """tasks: list of dicts with total, syscalls, pid, runnable.
    Returns index of the runnable task with min score."""
    w_total, w_sys, w_idle, w_others = weights
    best, best_score = None, None
    for i, t in enumerate(tasks):
        if not t["runnable"]:
            continue
        total_others = sum(u["total"] for j, u in enumerate(tasks)
                           if j != i and u["runnable"])
        s = (w_total * t["total"] + w_sys * t["syscalls"] +
             w_idle * (1 if t["pid"] == 1 else 0) + w_others * total_others)
        if best_score is None or s < best_score:
            best_score, best = s, i
    return best


def simulate_throughput(weights, B, bench_syscalls):
    tasks = [
        {"pid": 1, "total": 1000, "syscalls": 0, "runnable": True},  # idle (banked)
        {"pid": 4, "total": 0, "syscalls": 0, "runnable": True},   # benchmark
    ]
    steps = 0
    max_steps = 10 * B + 1000
    while tasks[1]["total"] < B and steps < max_steps:
        c = score_candidates(weights, tasks)
        tasks[c]["total"] += 1
        # benchmark syscalls grow ~linearly with progress
        tasks[1]["syscalls"] = int(bench_syscalls * tasks[1]["total"] / B)
        steps += 1
    return steps, steps / B


def simulate_arrival(weights, B_a, B_b, t_arrive):
    tasks = [
        {"pid": 1, "total": 1000, "syscalls": 0, "runnable": True},  # idle (banked)
        {"pid": 4, "total": 0, "syscalls": 50, "runnable": True},  # task A
        {"pid": 5, "total": 0, "syscalls": 50, "runnable": False},  # task B
    ]
    steps = 0
    max_steps = 10 * (B_a + B_b) + 2000
    while steps < max_steps:
        if steps == t_arrive:
            tasks[2]["runnable"] = True
        if tasks[1]["total"] >= B_a and tasks[2]["total"] >= B_b:
            break
        # completed tasks leave the runnable set
        for t, B in ((tasks[1], B_a), (tasks[2], B_b)):
            if t["total"] >= B:
                t["runnable"] = False
        c = score_candidates(weights, tasks)
        tasks[c]["total"] += 1
        # cost = runnable others
        steps += 1
    # normalized: optimal ~ (B_a + B_b) with 2 runnable avg; RR ~ 2x
    done = tasks[1]["total"] >= B_a and tasks[2]["total"] >= B_b
    norm = steps / (B_a + B_b) if done else 10.0  # heavy penalty for starvation
    return steps, norm


def objective(weights):
    costs = []
    for name, kind, p in WORKLOADS:
        if kind == "throughput":
            _, norm = simulate_throughput(weights, p["B"], p["syscalls"])
        else:
            _, norm = simulate_arrival(weights, p["B_a"], p["B_b"], p["t_arrive"])
        costs.append(norm)
    return float(np.mean(costs)), costs


def main():
    rng = np.random.default_rng(1)
    best_w, best_obj = None, float("inf")

    print("baselines: RR=2.0 on throughput (optimal 1.0); arrival penalizes starvation")

    N = 12000
    for i in range(N):
        w = rng.uniform(-3, 3, size=4)
        obj, _ = objective(w)
        if obj < best_obj:
            best_obj, best_w = obj, w.copy()
    print(f"random search ({N}): obj={best_obj:.3f}")

    step = 0.2
    w = best_w.copy()
    for it in range(600):
        improved = False
        for d in range(4):
            for sgn in (-1, 1):
                w2 = w.copy()
                w2[d] += sgn * step
                obj, _ = objective(w2)
                if obj < best_obj:
                    best_obj, best_w = obj, w2.copy()
                    w = w2.copy()
                    improved = True
        if not improved:
            step *= 0.7
            if step < 1e-4:
                break
    print(f"after hill climb: obj={best_obj:.3f}")

    print("weights (minimize score):")
    for name, val in zip(FEATS, best_w):
        print(f"    {name:12s} {val:+.4f}")
    _, costs = objective(best_w)
    print("per-workload normalized cost:")
    for (name, _, _), c in zip(WORKLOADS, costs):
        print(f"    {name:12s} {c:.3f}")

    np.save("/tmp/ml_weights.npy", best_w)
    print("saved /tmp/ml_weights.npy")


if __name__ == "__main__":
    main()
