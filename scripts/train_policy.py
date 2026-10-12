#!/usr/bin/env python3
"""Train a linear scheduling-cost model from MyOS `trace full` CSVs.

Approach: fitted-Q / Monte-Carlo return regression (offline RL).
  - Each trace record is a decision point: state = task snapshots,
    choice = next_idx (what the behavior policy RR/WF chose).
  - Interval cost c_t = (runnable_t - 1) * (tick[t+1] - tick[t]):
    total task x ticks of delay imposed on runnable-but-not-chosen tasks.
  - Return R_t = sum_{k>=t} gamma^(k-t) * c_k  (gamma=0.99).
  - Train Ridge regression: R_t ~= w . phi(state_t, choice_t).
  - Deployed policy is greedy: pick the runnable task minimizing w . phi.

NOTE (Oct 2026): this regression was found to learn spurious time trends
(total_ticks as a clock) because single-step Q differences (~1 tick) drown
in return noise. See scripts/sim_search.py for the simulator + policy-search
approach that superseded it. Kept for the dataset pipeline and diagnostics.

Features phi(state, choice c) -- all integer-computable in the kernel:
    wait_c       wait_ticks of the candidate
    total_c      total_ticks of the candidate
    syscalls_c   syscall_count of the candidate
    idle_c       1 if syscalls_c == 0 else 0   (idle-task indicator)
    n_other      # other runnable tasks
    wait_others  sum of wait_ticks of other runnable tasks
    total_others sum of total_ticks of other runnable tasks

Usage:
    python3 scripts/train_policy.py <trace1.csv> [<trace2.csv> ...]
        --out-weights src/kernel/proc/policy_ml_weights.h
        [--gamma 0.99] [--alpha 1.0]
"""
import argparse
import sys

import numpy as np
import pandas as pd
from sklearn.linear_model import Ridge
from sklearn.preprocessing import StandardScaler

READY, RUNNING = 0, 1
FIXED_SCALE = 20  # 2^20 fixed point for kernel weights


def load_decisions(path, gamma):
    df = pd.read_csv(path)
    nslots = max(int(c.split("_")[1]) for c in df.columns if c.startswith("pid_")) + 1

    ticks = df["tick"].to_numpy()
    choices = df["next_idx"].to_numpy()
    n = len(df)

    runnable = np.zeros(n, dtype=int)
    for s in range(nslots):
        st = df[f"state_{s}"].to_numpy()
        pid = df[f"pid_{s}"].to_numpy()
        runnable += ((st == READY) | (st == RUNNING)) & (pid > 0)

    dt = np.diff(ticks)
    dt = np.maximum(dt, 1)
    costs = (runnable[:-1] - 1).clip(min=0) * dt

    returns = np.zeros(n - 1)
    acc = 0.0
    for t in range(n - 2, -1, -1):
        acc = costs[t] + gamma * acc
        returns[t] = acc

    feats = []
    for t in range(n - 1):
        c = int(choices[t])
        if c >= nslots or df[f"pid_{c}"].iloc[t] <= 0:
            feats.append(None)
            continue
        wait_c = float(df[f"wait_{c}"].iloc[t])
        total_c = float(df[f"total_{c}"].iloc[t])
        syscalls_c = float(df[f"syscalls_{c}"].iloc[t])
        idle_c = 1.0 if syscalls_c == 0 else 0.0
        n_other = 0
        wait_others = 0.0
        total_others = 0.0
        for s in range(nslots):
            if s == c:
                continue
            pid = df[f"pid_{s}"].iloc[t]
            st = df[f"state_{s}"].iloc[t]
            if pid > 0 and st in (READY, RUNNING):
                n_other += 1
                wait_others += float(df[f"wait_{s}"].iloc[t])
                total_others += float(df[f"total_{s}"].iloc[t])
        feats.append([wait_c, total_c, syscalls_c, idle_c,
                      float(n_other), wait_others, total_others])

    feat_names = ["wait_c", "total_c", "syscalls_c", "idle_c",
                  "n_other", "wait_others", "total_others"]
    rows = [(f, r) for f, r in zip(feats, returns) if f is not None]
    X = np.array([r[0] for r in rows])
    y = np.array([r[1] for r in rows])
    return X, y, feat_names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("traces", nargs="+")
    ap.add_argument("--out-weights", required=True)
    ap.add_argument("--gamma", type=float, default=0.99)
    ap.add_argument("--alpha", type=float, default=1.0)
    args = ap.parse_args()

    Xs, ys = [], []
    for p in args.traces:
        X, y, feat_names = load_decisions(p, args.gamma)
        print(f"train: {p}: {len(y)} decisions, "
              f"mean_return={y.mean():.1f} max_return={y.max():.1f}")
        Xs.append(X)
        ys.append(y)
    X = np.vstack(Xs)
    y = np.concatenate(ys)
    print(f"train: total {len(y)} decisions, {X.shape[1]} features")

    scaler = StandardScaler()
    Xs_ = scaler.fit_transform(X)
    model = Ridge(alpha=args.alpha)
    model.fit(Xs_, y)
    r2 = model.score(Xs_, y)
    print(f"train: Ridge(alpha={args.alpha}) R^2={r2:.3f}")

    print("train: standardized weights (negative = prefer):")
    for name, w in zip(feat_names, model.coef_):
        print(f"    {name:12s} {w:+.4f}")
    print(f"    {'intercept':12s} {model.intercept_:+.4f}")

    w_raw = model.coef_ / scaler.scale_
    b_raw = model.intercept_ - np.sum(model.coef_ * scaler.mean_ / scaler.scale_)

    w_fixed = np.round(w_raw * (1 << FIXED_SCALE)).astype(np.int64)
    b_fixed = int(round(b_raw * (1 << FIXED_SCALE)))

    with open(args.out_weights, "w") as f:
        f.write("/* Auto-generated by scripts/train_policy.py -- do not edit. */\n")
        f.write(f"/* scale: 2^{FIXED_SCALE} fixed point. score = b + sum w_i*x_i. */\n")
        f.write("#ifndef POLICY_ML_WEIGHTS_H\n#define POLICY_ML_WEIGHTS_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define MLW_SCALE {FIXED_SCALE}\n")
        f.write(f"#define MLW_BIAS {int(b_fixed)}LL\n")
        f.write("static const int64_t mlw_weights[7] = {\n")
        for name, wf in zip(feat_names, w_fixed):
            f.write(f"    {int(wf)}LL, /* {name} */\n")
        f.write("};\n\n#endif\n")
    print(f"train: weights -> {args.out_weights}")


if __name__ == "__main__":
    main()
