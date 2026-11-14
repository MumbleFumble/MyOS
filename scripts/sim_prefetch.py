#!/usr/bin/env python3
"""Offline prefetch-policy simulator for MyOS Phase 3 (learned prefetching).

Replays a demand LBA stream through a simulated block cache with the same
semantics as src/kernel/fs/bcache.c:

  * fully associative, BCACHE_SIZE entries, LRU eviction
  * demand miss inserts at MRU; prefetch inserts at the LRU end, flagged
    "prefetched unused" (a hit on a flagged block = useful prefetch; an
    eviction of a flagged block = wasted prefetch)
  * pollution: a demand miss that would have hit in a shadow cache that
    never prefetches (the sim's shadow is exact; the kernel's is an
    approximation that only updates on misses)

Policies: none, sequential and stride mirror src/kernel/fs/prefetch.c
exactly (4-ahead on trigger). oracle looks ahead in the trace (ceiling).
learned scores candidates with a linear model (see features below).

Trace input: one LBA per line, or a MyOS serial log containing
"ACC seq=<n> lba=<n> hit=<0|1>" lines from the acctrace shell command.

Synthetic workloads (--synthetic NAME):
  seq        sequential scan
  stride     constant stride 4
  interleave two sequential streams alternated (breaks both baselines:
             the global last-delta never repeats, single-stream state
             is never kept)
  mixed      phases: sequential, stride 8, interleave, sequential
  zipf       repeated hot working set with cold scans

Usage:
  sim_prefetch.py --trace FILE [--weights prefetch_weights.h]
  sim_prefetch.py --synthetic interleave [--weights ...]
"""

import argparse
import math
import re
import sys

CACHE_SIZE = 64
READ_US = 42  # measured PIO sector read latency, Phase 3 inc.2


class Cache:
    def __init__(self, policy):
        self.policy = policy
        self.order = []            # LRU .. MRU
        self.present = set()
        self.unused = set()        # present and prefetched, not yet hit
        self.shadow = []           # demand-only LRU order
        self.shadow_present = set()
        self.hits = 0
        self.misses = 0
        self.issued = 0
        self.useful = 0
        self.wasted = 0
        self.pollution = 0

    def _evict_lru(self):
        victim = self.order.pop(0)
        self.present.discard(victim)
        if victim in self.unused:
            self.unused.discard(victim)
            self.wasted += 1
            self.policy.note_outcome(False)

    def _shadow_access(self, lba):
        """Update the no-prefetch shadow; return True if it would hit."""
        if lba in self.shadow_present:
            self.shadow.remove(lba)
            self.shadow.append(lba)
            return True
        if len(self.shadow) >= CACHE_SIZE:
            old = self.shadow.pop(0)
            self.shadow_present.discard(old)
        self.shadow.append(lba)
        self.shadow_present.add(lba)
        return False

    def demand(self, lba):
        hit = lba in self.present
        shadow_hit = self._shadow_access(lba)
        if hit:
            self.hits += 1
            self.order.remove(lba)
            self.order.append(lba)
            if lba in self.unused:
                self.unused.discard(lba)
                self.useful += 1
                self.policy.note_outcome(True)
        else:
            self.misses += 1
            if shadow_hit:
                self.pollution += 1
            if len(self.order) >= CACHE_SIZE:
                self._evict_lru()
            self.order.append(lba)
            self.present.add(lba)
        self.policy.on_access(self, lba, hit)
        return hit

    def prefetch(self, lba):
        if lba in self.present or lba < 0:
            return
        self.issued += 1
        if len(self.order) >= CACHE_SIZE:
            self._evict_lru()
        self.order.insert(0, lba)  # LRU end, like bcache_prefetch_insert
        self.present.add(lba)
        self.unused.add(lba)

    def prefetch_mru(self, lba):
        """Insert at the MRU end. Used by the learned policy: its score is
        a confidence estimate, and Phase 3 measurements showed LRU-end
        insertion is self-defeating under a full cache (each new prefetch
        makes the previous, not-yet-demanded prefetch the next eviction
        victim, so useful depth collapses to ~the cache size and then
        thrashes). MRU insertion keeps a scored prefetch alive until its
        demand arrives; a wrong one still ages out through normal LRU."""
        if lba in self.present or lba < 0:
            return
        self.issued += 1
        if len(self.order) >= CACHE_SIZE:
            self._evict_lru()
        self.order.append(lba)
        self.present.add(lba)
        self.unused.add(lba)


class Policy:
    name = "base"

    def reset(self):
        pass

    def note_outcome(self, useful):
        pass

    def on_access(self, cache, lba, hit):
        pass


class NonePolicy(Policy):
    name = "none"


class SequentialPolicy(Policy):
    name = "sequential"

    def reset(self):
        self.has_last = False
        self.last = 0

    def on_access(self, cache, lba, hit):
        if self.has_last and lba == self.last + 1:
            for i in range(1, 5):
                cache.prefetch(lba + i)
        self.last = lba
        self.has_last = True


class StridePolicy(Policy):
    name = "stride"

    def reset(self):
        self.hist = []
        self.count = 0

    def on_access(self, cache, lba, hit):
        self.hist.append(lba)
        self.hist = self.hist[-3:]
        if self.count < 3:
            self.count += 1
        if self.count >= 3 and len(self.hist) == 3:
            s1 = self.hist[1] - self.hist[0]
            s2 = self.hist[2] - self.hist[1]
            if s1 == s2 and s1 != 0:
                for i in range(1, 5):
                    cache.prefetch(lba + s2 * i)


class OraclePolicy(Policy):
    """Perfect knowledge: prefetch not-yet-cached blocks demanded within
    the next LOOKAHEAD accesses (at most 4 per access). Ceiling only."""

    name = "oracle"
    LOOKAHEAD = 16

    def set_trace(self, trace):
        self.trace = trace

    def on_access(self, cache, lba, hit):
        i = self.idx
        issued = 0
        seen = set()
        for j in range(i + 1, min(i + 1 + self.LOOKAHEAD, len(self.trace))):
            t = self.trace[j]
            if t in seen:
                continue
            seen.add(t)
            if t not in cache.present:
                cache.prefetch_mru(t)  # ceiling assumes scored insertion
                issued += 1
                if issued >= 4:
                    break


# --- learned policy: candidate generation + linear scoring ---------------

FEATURES = [
    "bias",
    "is_next",           # candidate is lba+1
    "stride_confirmed",  # offset == d1 == d2 (two equal deltas in a row)
    "stride_unconf",     # offset == d1 but d1 != d2
    "seq_run",           # min(seq run length,4)/4, offsets 1..4 only
    "dist",              # 1/(1+floor(log2(|offset|))): prefer near blocks
    "recent_acc",        # useful/(useful+wasted) over last 32 outcomes
    "reuse",             # target demanded within the last 512 accesses
    "jump",              # |d1| > 4: stream just jumped (all candidates)
    "stream_cont",       # offset == 1 and lba continues a stream seen
                         # 2..8 accesses ago (lba == that lba + 1):
                         # interleaved sequential streams, any count
]


class LearnedPolicy(Policy):
    name = "learned"

    def __init__(self, weights, threshold=0.0):
        self.weights = weights
        self.threshold = threshold

    def reset(self):
        self.prev1 = None
        self.prev2 = None
        self.hist = []           # last 8 accessed lbas, oldest first
        self._lba_now = None
        self.seq_run = 0
        self.outcomes = []       # last 32 booleans
        self.last_seen = {}      # lba -> access index
        self.index = 0

    def note_outcome(self, useful):
        self.outcomes.append(useful)
        self.outcomes = self.outcomes[-32:]

    def _features(self, offset, target, d1, d2):
        f = {}
        f["bias"] = 1.0
        f["is_next"] = 1.0 if offset == 1 else 0.0
        f["stride_confirmed"] = 1.0 if (d1 is not None and offset == d1 == d2 and d1 != 0) else 0.0
        f["stride_unconf"] = 1.0 if (d1 is not None and offset == d1 and d1 != d2 and d1 != 0) else 0.0
        f["seq_run"] = (min(self.seq_run, 4) / 4.0) if 1 <= offset <= 4 else 0.0
        a = abs(offset)
        f["dist"] = 1.0 / (1 + (a.bit_length() - 1)) if a >= 1 else 0.0
        if self.outcomes:
            f["recent_acc"] = sum(self.outcomes) / len(self.outcomes)
        else:
            f["recent_acc"] = 0.5
        last = self.last_seen.get(target)
        f["reuse"] = 1.0 if (last is not None and self.index - last <= 512) else 0.0
        f["jump"] = 1.0 if (d1 is not None and abs(d1) > 4) else 0.0
        f["stream_cont"] = 0.0
        if offset == 1 and self._lba_now is not None:
            # hist holds previous accesses; hist[-1] is the immediately
            # previous one (plain sequential, covered by other features),
            # so look at accesses 2..8 back for a +1 continuation.
            for h in self.hist[:-1]:
                if self._lba_now - h == 1:
                    f["stream_cont"] = 1.0
                    break
        return f

    def on_access(self, cache, lba, hit):
        self.index += 1
        self._lba_now = lba
        self.last_seen[lba] = self.index
        d1 = (lba - self.prev1) if self.prev1 is not None else None
        d2 = (self.prev1 - self.prev2) if (self.prev1 is not None and self.prev2 is not None) else None
        if d1 == 1:
            self.seq_run += 1
        else:
            self.seq_run = 0

        candidates = {1, 2, 3, 4}
        if d1:
            candidates.add(d1)
            candidates.add(2 * d1)
        if d2:
            candidates.add(d2)
        for off in sorted(candidates):
            if off <= 0 or off > 64:
                continue
            target = lba + off
            feats = self._features(off, target, d1, d2)
            score = sum(self.weights.get(k, 0.0) * v for k, v in feats.items())
            if score > self.threshold:
                cache.prefetch_mru(target)

        self.hist.append(lba)
        self.hist = self.hist[-8:]
        self.prev2 = self.prev1
        self.prev1 = lba


def run_trace(trace, policy):
    cache = Cache(policy)
    policy.reset()
    if isinstance(policy, OraclePolicy):
        policy.set_trace(trace)
    for i, lba in enumerate(trace):
        if isinstance(policy, OraclePolicy):
            policy.idx = i
        cache.demand(lba)
    return cache


def cost(cache):
    """Objective: demand misses (stalls) + wasted prefetches (useless disk
    reads). Useful prefetches are treated as free (latency hidden)."""
    return cache.misses + cache.wasted


def report(name, cache, base_misses):
    saved = base_misses - cache.misses
    print(f"{name:<12} hits={cache.hits:<6} misses={cache.misses:<6} "
          f"issued={cache.issued:<6} useful={cache.useful:<6} "
          f"wasted={cache.wasted:<6} pollution={cache.pollution:<6} "
          f"cost={cost(cache):<6} misses_avoided={saved}")


def load_trace(path):
    lbas = []
    acc_re = re.compile(r"ACC seq=\d+ lba=(\d+)")
    with open(path) as f:
        for line in f:
            m = acc_re.search(line)
            if m:
                lbas.append(int(m.group(1)))
                continue
            line = line.strip()
            if line.isdigit():
                lbas.append(int(line))
    return lbas


def synthetic(name):
    if name == "seq":
        return list(range(200, 2200))
    if name == "stride":
        return [100 + 4 * i for i in range(2000)]
    if name == "stride3":
        return [7000 + 3 * i for i in range(1800)]
    if name == "stride16":
        return [20000 + 16 * i for i in range(1200)]
    if name == "interleave":
        out = []
        for i in range(1000):
            out.append(1000 + i)
            out.append(5000 + i)
        return out
    if name == "mixed":
        out = list(range(200, 1000))
        out += [2000 + 8 * i for i in range(400)]
        for i in range(400):
            out.append(100 + i)
            out.append(9000 + i)
        out += list(range(300, 1100))
        return out
    if name == "zipf":
        import random
        rng = random.Random(42)
        hot = list(range(500, 532))
        out = []
        for i in range(3000):
            if rng.random() < 0.8:
                out.append(rng.choice(hot))
            else:
                out.append(2000 + (i % 700))
        return out
    raise SystemExit(f"unknown synthetic workload: {name}")


def load_weights(path):
    """Parse a kernel-style weights header (#define PW_<FEATURE> <int>,
    fixed point x1024) or a plain key=value text file."""
    w = {}
    define_re = re.compile(r"#define\s+PW_([A-Z_]+)\s+(-?\d+)")
    kv_re = re.compile(r"^([a-z_]+)\s*=\s*(-?[\d.]+)$")
    with open(path) as f:
        for line in f:
            m = define_re.search(line)
            if m:
                w[m.group(1).lower()] = int(m.group(2)) / 1024.0
                continue
            m = kv_re.match(line.strip())
            if m:
                w[m.group(1)] = float(m.group(2))
    thr = w.pop("threshold", 0.0)
    return w, thr


def main():
    global CACHE_SIZE
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace")
    ap.add_argument("--synthetic")
    ap.add_argument("--weights", help="weights file for the learned policy")
    ap.add_argument("--cache-size", type=int, default=CACHE_SIZE)
    args = ap.parse_args()

    CACHE_SIZE = args.cache_size

    if args.synthetic:
        trace = synthetic(args.synthetic)
        label = f"synthetic:{args.synthetic}"
    elif args.trace:
        trace = load_trace(args.trace)
        label = f"trace:{args.trace}"
    else:
        ap.error("give --trace or --synthetic")
    print(f"{label}: {len(trace)} demand accesses, cache={CACHE_SIZE}")

    base = run_trace(trace, NonePolicy())
    policies = [NonePolicy(), SequentialPolicy(), StridePolicy(), OraclePolicy()]
    if args.weights:
        w, thr = load_weights(args.weights)
        policies.insert(3, LearnedPolicy(w, thr))
    for p in policies:
        report(p.name, run_trace(trace, p), base.misses)


if __name__ == "__main__":
    main()
