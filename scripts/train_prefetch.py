#!/usr/bin/env python3
"""Train the learned prefetch policy's linear weights (Phase 3, inc.6).

Coordinate search over the weights in sim_prefetch.LearnedPolicy,
minimising mean cost (demand misses + wasted prefetches) across the
training workloads. Held-out workloads (different bases, strides and
phase orders, never trained on) are evaluated at the end with all
policies for a generalisation check.

Outputs:
  scripts/prefetch_weights.txt        floats, loadable by sim_prefetch.py
  src/kernel/fs/prefetch_weights.h    fixed-point (x1024) header for the
                                      kernel port (inc.7)

Usage: train_prefetch.py [--serial LOG]  (optionally add a real acctrace
       serial log to the training set)
"""

import argparse
import random
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sim_prefetch as sp


def heldout(name):
    if name == "interleave3":
        # Three interleaved sequential streams, unseen bases.
        out = []
        for i in range(700):
            out.append(3000 + i)
            out.append(8000 + i)
            out.append(15000 + i)
        return out
    if name == "stride7":
        return [400 + 7 * i for i in range(1800)]
    if name == "mixed2":
        # Different phase order: stride first, then interleave, then seq.
        out = [5000 + 8 * i for i in range(400)]
        for i in range(500):
            out.append(700 + i)
            out.append(12000 + i)
        out += list(range(2000, 2900))
        return out
    if name == "seq_far":
        return list(range(40000, 42000))
    raise SystemExit(name)


TRAIN = ["seq", "stride", "stride3", "stride16", "interleave", "mixed", "zipf"]
HELDOUT = ["interleave3", "stride7", "mixed2", "seq_far"]

# Starting point: the best weights from the first training run (uniform
# strides only). The ratio objective below refines from here.
INIT = {
    "bias": -1.0,
    "is_next": 0.9,
    "stride_confirmed": 1.0,
    "stride_unconf": 0.3,
    "seq_run": 0.3,
    "dist": 0.2,
    "recent_acc": 0.5,
    "reuse": 0.95,
    "jump": -0.3,
    "stream_cont": 1.2,
}


def mean_cost(weights, thr, traces, base_costs=None):
    """Objective: mean per-trace cost ratio (cost / no-prefetch cost),
    plus a penalty on the worst ratio. A plain mean of raw costs lets
    the optimiser sacrifice one workload class entirely (observed: it
    zeroed the interleaved-stream behaviour to win on strides)."""
    pol = sp.LearnedPolicy(weights, thr)
    ratios = []
    for i, t in enumerate(traces):
        c = sp.cost(sp.run_trace(t, pol))
        base = base_costs[i] if base_costs else len(t)
        ratios.append(c / max(base, 1))
    mean = sum(ratios) / len(ratios)
    return mean + 0.25 * (max(ratios) - mean)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", help="MyOS serial log with ACC lines, added to training set")
    args = ap.parse_args()

    traces = [sp.synthetic(n) for n in TRAIN]
    names = list(TRAIN)
    if args.serial:
        real = sp.load_trace(args.serial)
        if real:
            traces.append(real)
            names.append(f"serial:{len(real)}")
    print(f"training on: {', '.join(names)}")

    base_costs = [sp.cost(sp.run_trace(t, sp.NonePolicy())) for t in traces]
    weights = dict(INIT)
    thr = 0.0
    best = mean_cost(weights, thr, traces, base_costs)
    print(f"init objective: {best:.3f}")

    rng = random.Random(7)
    for step in (0.5, 0.25, 0.1):
        improved = True
        rounds = 0
        while improved and rounds < 3:
            improved = False
            rounds += 1
            for k in list(weights):
                for delta in (step, -step):
                    trial = dict(weights)
                    trial[k] = round(trial[k] + delta, 4)
                    c = mean_cost(trial, thr, traces, base_costs)
                    if c < best - 1e-9:
                        weights, best = trial, c
                        improved = True
            for delta in (step / 2, -step / 2):
                c = mean_cost(weights, thr + delta, traces, base_costs)
                if c < best - 1e-9:
                    thr = round(thr + delta, 4)
                    best = c
                    improved = True
        print(f"step {step}: objective {best:.3f}")

    print("\nlearned weights:", {k: weights[k] for k in sp.FEATURES if k in weights},
          "threshold:", thr)

    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "prefetch_weights.txt"), "w") as f:
        f.write("# Learned prefetch weights, trained by train_prefetch.py\n")
        for k in sp.FEATURES:
            f.write(f"{k} = {weights.get(k, 0.0)}\n")
        f.write(f"threshold = {thr}\n")

    hdr = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                       "src", "kernel", "fs", "prefetch_weights.h")
    with open(hdr, "w") as f:
        f.write("/* Learned prefetch weights, generated by scripts/train_prefetch.py.\n")
        f.write(" * Fixed point x1024. Do not edit by hand; re-run the trainer. */\n")
        f.write("#pragma once\n\n")
        for k in sp.FEATURES:
            f.write(f"#define PW_{k.upper():<18} {int(round(weights.get(k, 0.0) * 1024))}\n")
        f.write(f"#define PW_THRESHOLD         {int(round(thr * 1024))}\n")
    print(f"wrote prefetch_weights.txt and {os.path.normpath(hdr)}")

    print("\n--- held-out evaluation ---")
    for name in HELDOUT:
        t = heldout(name)
        base = sp.run_trace(t, sp.NonePolicy())
        print(f"{name}: {len(t)} accesses, none cost={sp.cost(base)}")
        for p in [sp.SequentialPolicy(), sp.StridePolicy(),
                  sp.LearnedPolicy(weights, thr), sp.OraclePolicy()]:
            c = sp.run_trace(t, p)
            print(f"  {p.name:<12} misses={c.misses:<6} useful={c.useful:<6} "
                  f"wasted={c.wasted:<6} cost={sp.cost(c)}")


if __name__ == "__main__":
    main()
