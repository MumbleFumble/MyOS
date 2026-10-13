#!/usr/bin/env python3
"""
Phase 2 policy search (increment 5).

Replays the deterministic bench_pf access pattern through a 4-frame
simulated cache. Searches for linear eviction-policy weights that
minimize total page faults, comparing against FIFO and OPT (Belady's).

Model: each resident frame gets an eviction score; the highest score
is evicted on a fault when no frame is free.

    score = w_age * age + w_freq * freq

  age  = number of faults since this frame was allocated (residency age)
  freq = total faults seen for this page so far (hotness)

Features are standardized using stats from a FIFO pilot run. Search is
random sampling followed by hill climbing. The winner is validated on
a held-out pattern (different LCG seed) to check for overfitting.

Usage:
    python3 scripts/sim_pf_search.py
Prints baseline fault counts, learned weights, and validation results.
Writes scripts/pf_weights.txt for the kernel fixed-point conversion.
"""

import random
import math

N_FRAMES = 4

# ---------------------------------------------------------------------------
# Access pattern (must match user/bench_pf.c exactly).
# ---------------------------------------------------------------------------

def lcg(seed):
    state = seed
    while True:
        state = (state * 1103515245 + 12345) & 0xFFFFFFFFFFFFFFFF
        yield (state >> 16) & 0x7FFF

def gen_pattern(seed=12345):
    acc = []
    # Phase A: sequential scan 0..7 twice.
    for _ in range(2):
        acc.extend(range(8))
    # Phase B: hot loop 0..5, 20 iterations.
    for _ in range(20):
        acc.extend(range(6))
    # Phase C: 80/20 hot/cold via LCG.
    g = lcg(seed)
    for _ in range(100):
        v = next(g) % 100
        if v < 80:
            acc.append(next(g) % 4)          # hot: 0..3
        else:
            acc.append(8 + next(g) % 8)      # cold: 8..15
    return acc

TRAIN = gen_pattern(12345)
HELDOUT = gen_pattern(99999)
assert len(TRAIN) == 236, len(TRAIN)

# ---------------------------------------------------------------------------
# Simulator.
# ---------------------------------------------------------------------------

class Cache:
    """4-frame fully-associative cache with pluggable eviction policy."""
    def __init__(self, policy_fn):
        self.policy_fn = policy_fn
        self.frames = []          # list of page numbers (resident)
        self.alloc_seq = {}       # page -> allocation sequence
        self.fault_count = {}     # page -> total faults
        self.seq = 0
        self.faults = 0

    def access(self, page):
        if page in self.frames:
            return  # hit
        self.faults += 1
        self.fault_count[page] = self.fault_count.get(page, 0) + 1
        if len(self.frames) < N_FRAMES:
            self.frames.append(page)
        else:
            victim = self.policy_fn(self, page)
            self.frames.remove(victim)
            self.frames.append(page)
        self.alloc_seq[page] = self.seq
        self.seq += 1

def fifo_policy(cache, page):
    """Evict lowest allocation sequence (longest resident)."""
    return min(cache.frames, key=lambda p: cache.alloc_seq[p])

def opt_policy_factory(future):
    """Belady's OPT: evict page whose next use is furthest in future."""
    pos = 0
    def policy(cache, page):
        nonlocal pos
        # future[pos] is the current access; look ahead from pos+1.
        best, best_dist = None, -1
        for p in cache.frames:
            try:
                dist = future.index(p, pos + 1)
            except ValueError:
                return p  # never used again: evict immediately
            if dist > best_dist:
                best, best_dist = p, dist
        return best
    def advance():
        nonlocal pos
        pos += 1
    policy.advance = advance
    return policy

def run(pattern, policy_fn, advance=None):
    c = Cache(policy_fn)
    for p in pattern:
        c.access(p)
        if advance:
            advance()
    return c.faults

# ---------------------------------------------------------------------------
# Learned linear policy.
# ---------------------------------------------------------------------------

# Standardization stats from a FIFO pilot run (computed below).
MEAN_AGE, STD_AGE = 1.0, 1.0
MEAN_FREQ, STD_FREQ = 1.0, 1.0

def learned_policy_factory(w_age, w_freq):
    def policy(cache, page):
        best, best_score = None, None
        for p in cache.frames:
            age = cache.seq - cache.alloc_seq[p]
            freq = cache.fault_count.get(p, 0)
            a = (age - MEAN_AGE) / STD_AGE
            f = (freq - MEAN_FREQ) / STD_FREQ
            score = w_age * a + w_freq * f
            if best_score is None or score > best_score:
                best, best_score = p, score
        return best
    return policy

def evaluate(weights, pattern):
    w_age, w_freq = weights
    return run(pattern, learned_policy_factory(w_age, w_freq))

# ---------------------------------------------------------------------------
# Main: baselines, pilot stats, search, validation.
# ---------------------------------------------------------------------------

def main():
    global MEAN_AGE, STD_AGE, MEAN_FREQ, STD_FREQ

    # Baselines on train pattern.
    fifo_faults = run(TRAIN, fifo_policy)
    opt = opt_policy_factory(TRAIN)
    opt_faults = run(TRAIN, opt, opt.advance)
    print(f"train accesses: {len(TRAIN)}")
    print(f"FIFO faults: {fifo_faults}")
    print(f"OPT  faults: {opt_faults}")

    # Pilot run to collect feature stats for standardization.
    ages, freqs = [], []
    c = Cache(fifo_policy)
    for p in TRAIN:
        if p not in c.frames:
            # About to evict (or fill): record features of residents.
            if len(c.frames) == N_FRAMES:
                for q in c.frames:
                    ages.append(c.seq - c.alloc_seq[q])
                    freqs.append(c.fault_count.get(q, 0))
        c.access(p)
    MEAN_AGE = sum(ages) / len(ages)
    STD_AGE = math.sqrt(sum((a - MEAN_AGE) ** 2 for a in ages) / len(ages)) or 1.0
    MEAN_FREQ = sum(freqs) / len(freqs)
    STD_FREQ = math.sqrt(sum((f - MEAN_FREQ) ** 2 for f in freqs) / len(freqs)) or 1.0
    print(f"feature stats: age mean={MEAN_AGE:.1f} std={STD_AGE:.1f}, "
          f"freq mean={MEAN_FREQ:.1f} std={STD_FREQ:.1f}")

    # Random search.
    random.seed(42)
    best = None
    for _ in range(4000):
        w = (random.uniform(-3, 3), random.uniform(-3, 3))
        f = evaluate(w, TRAIN)
        if best is None or f < best[0]:
            best = (f, w)
    print(f"random search: faults={best[0]} w_age={best[1][0]:+.4f} w_freq={best[1][1]:+.4f}")

    # Hill climb.
    faults, (wa, wf) = best
    step = 0.2
    for _ in range(200):
        improved = False
        for dwa, dwf in [(step, 0), (-step, 0), (0, step), (0, -step)]:
            f = evaluate((wa + dwa, wf + dwf), TRAIN)
            if f < faults:
                faults, wa, wf = f, wa + dwa, wf + dwf
                improved = True
                break
        if not improved:
            step *= 0.5
            if step < 1e-4:
                break
    print(f"hill climb:    faults={faults} w_age={wa:+.4f} w_freq={wf:+.4f}")

    # Validation on held-out pattern.
    fifo_h = run(HELDOUT, fifo_policy)
    opt_h = opt_policy_factory(HELDOUT)
    opt_h_faults = run(HELDOUT, opt_h, opt_h.advance)
    learned_h = evaluate((wa, wf), HELDOUT)
    print(f"heldout: FIFO={fifo_h} OPT={opt_h_faults} learned={learned_h}")

    # Sanity: learned should beat FIFO on train, and not collapse on heldout.
    if faults >= fifo_faults:
        print("WARNING: learned did not beat FIFO on train!")
    if learned_h >= fifo_h:
        print("WARNING: learned did not beat FIFO on heldout (possible overfit)!")

    with open("scripts/pf_weights.txt", "w") as f:
        f.write(f"{wa:.6f} {wf:.6f}\n")
    print("weights written to scripts/pf_weights.txt")

if __name__ == "__main__":
    main()
