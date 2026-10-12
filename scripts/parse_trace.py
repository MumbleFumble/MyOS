#!/usr/bin/env python3
"""Parse MyOS `trace full` output from a QEMU serial log into a dataset CSV.

The shell's `trace full` command prints one REC line per drained switch
record followed by one TASK line per task-table slot:

    REC tick=<tick> prev=<prev_idx> next=<next_idx> ntasks=<n>
    TASK idx=<i> pid=<pid> state=<s> total=<total_ticks> wait=<wait_ticks>
         syscalls=<syscall_count> ioblocks=<io_block_count>

Usage:
    python3 scripts/parse_trace.py <serial_log> [--out <csv>] [--slots N]

Output: one row per switch record (decision point), with per-slot task
snapshots flattened into fixed-width columns (padded to --slots, default 8).
Absent slots get pid=-1, state=255, and zeros elsewhere.

The training script (train_policy.py) computes labels (interval costs and
returns) from consecutive records; this parser only extracts faithfully.
"""
import argparse
import csv
import re
import sys

REC_RE = re.compile(
    r"^REC tick=(\d+) prev=(\d+) next=(\d+) ntasks=(\d+)\s*$")
TASK_RE = re.compile(
    r"^TASK idx=(\d+) pid=(-?\d+) state=(\d+) total=(\d+) wait=(\d+) "
    r"syscalls=(\d+) ioblocks=(\d+)\s*$")

STATE_NAMES = {0: "READY", 1: "RUNNING", 2: "DEAD", 3: "WAITING"}


def parse(path):
    records = []
    cur = None
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.strip().lstrip(">").strip()
            m = REC_RE.match(line)
            if m:
                if cur is not None:
                    records.append(cur)
                cur = {
                    "tick": int(m.group(1)),
                    "prev_idx": int(m.group(2)),
                    "next_idx": int(m.group(3)),
                    "ntasks": int(m.group(4)),
                    "tasks": {},
                }
                continue
            m = TASK_RE.match(line)
            if m and cur is not None:
                idx = int(m.group(1))
                cur["tasks"][idx] = {
                    "pid": int(m.group(2)),
                    "state": int(m.group(3)),
                    "total": int(m.group(4)),
                    "wait": int(m.group(5)),
                    "syscalls": int(m.group(6)),
                    "ioblocks": int(m.group(7)),
                }
    if cur is not None:
        records.append(cur)
    return records


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("serial_log")
    ap.add_argument("--out", default=None)
    ap.add_argument("--slots", type=int, default=8)
    args = ap.parse_args()

    records = parse(args.serial_log)
    if not records:
        print("parse_trace: no REC lines found", file=sys.stderr)
        sys.exit(1)

    out_path = args.out or args.serial_log + ".trace.csv"
    nslots = args.slots
    cols = ["rec_idx", "tick", "prev_idx", "next_idx", "ntasks"]
    for s in range(nslots):
        cols += [f"pid_{s}", f"state_{s}", f"total_{s}", f"wait_{s}",
                 f"syscalls_{s}", f"ioblocks_{s}"]

    n_partial = 0
    with open(out_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(cols)
        for ri, r in enumerate(records):
            if len(r["tasks"]) != r["ntasks"]:
                n_partial += 1
            row = [ri, r["tick"], r["prev_idx"], r["next_idx"], r["ntasks"]]
            for s in range(nslots):
                t = r["tasks"].get(s)
                if t is None:
                    row += [-1, 255, 0, 0, 0, 0]
                else:
                    row += [t["pid"], t["state"], t["total"],
                            t["wait"], t["syscalls"], t["ioblocks"]]
            w.writerow(row)

    print(f"parse_trace: {len(records)} records -> {out_path} "
          f"({n_partial} with missing TASK lines)")


if __name__ == "__main__":
    main()
