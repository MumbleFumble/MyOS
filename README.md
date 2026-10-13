# MyOS — AI-Native OS Research Kernel

MyOS is an experimental research kernel exploring whether machine learning policies
can replace or outperform classical heuristics in operating system resource management.

The project is a research prototype, not a general-purpose operating system. Its primary
purpose is to serve as a platform for studying learned, adaptive kernel decision-making
in a controlled, measurable environment.

---

## Research Motivation

Classical operating systems manage resources — CPU time, memory pages, I/O queues —
using hand-written heuristics: round-robin scheduling, LRU page replacement, deadline
I/O prioritization. These heuristics are well-studied, but they are static. They do
not adapt to workload characteristics, hardware topology, or application behavior over time.

The central research question this project investigates is:

> **Can learned policies, trained on system telemetry, make better resource management
> decisions than classical OS heuristics — and can those improvements be measured?**

This is an open problem in systems research. Recent work in ML-for-systems (e.g. learned
index structures, neural network-based cache eviction, RL-driven compilers) suggests
that the premise is plausible in narrow domains. This kernel is an attempt to study it
at the OS policy layer, where decisions have direct, measurable effects on latency,
throughput, and resource utilization.

---

## System Overview

MyOS is structured around a strict three-layer separation:

```
┌────────────────────────────────────────────┐
│           Deterministic Kernel Core        │
│  (memory, interrupts, syscalls, I/O, ELF)  │
└────────────────┬───────────────────────────┘
                 │ exposes structured telemetry
                 ▼
┌────────────────────────────────────────────┐
│         Policy Interface Layer             │
│  (pluggable: heuristic or learned model)   │
└────────────────┬───────────────────────────┘
                 │ emits decisions
                 ▼
┌────────────────────────────────────────────┐
│         Resource Dispatch                  │
│  (scheduler, MMU, I/O queue manager)       │
└────────────────────────────────────────────┘
```

**Deterministic kernel core** — handles hardware initialization, interrupt routing,
virtual memory, ELF loading, and the syscall ABI. This layer is not subject to learned
policies and is kept stable for reproducible benchmarking.

**Policy interface layer** — a narrow, well-defined interface through which resource
management decisions are made. Any conforming policy (a classical algorithm or a
trained model) can be plugged in without modifying the kernel core. A baseline
deterministic policy is always available as the reference implementation.

**Resource dispatch** — executes the decisions emitted by the policy layer: selecting
the next process to run, choosing a page to evict, ordering I/O requests. This layer
records outcomes as telemetry, which feeds back into the policy evaluation loop.

---

## Current Kernel Status

The kernel currently implements a working deterministic foundation:

- 64-bit x86 boot via GRUB/Multiboot
- 4-level paging, virtual memory, kernel heap
- Physical memory manager (PMM)
- Preemptive round-robin scheduler with per-task CR3
- IDT, IRQ routing, PIC, PIT timer
- `int 0x80` syscall interface (ring-3 trap gate)
- ELF64 loader with private per-process address spaces
- VGA terminal driver, PS/2 keyboard driver
- RTC driver
- ATA PIO disk driver
- Simple in-kernel VFS with ramfs and MyFS disk backend
- User-space malloc, shell, basic utilities (`ls`, `cat`, `date`, `exec`)

This deterministic baseline is the comparison target against which all learned
policies will be evaluated.

---

## Research Roadmap

### Phase 1 — AI-Driven CPU Scheduler (active focus)

The scheduler is the first subsystem targeted for learned policy replacement.

**Goal:** Replace the round-robin scheduling heuristic with a policy that observes
per-task telemetry (run time, wait time, I/O blocking rate, syscall frequency) and
learns to assign CPU quanta to minimize a defined cost function (e.g. mean latency,
throughput, fairness index).

**Approach:**
- Instrument the scheduler to emit per-context-switch telemetry records
- Define a policy interface: given current run-queue state, emit a `next_task` decision
- Implement a baseline deterministic policy (round-robin, CFS-approximation)
- Train an offline model on recorded telemetry traces
- Evaluate the learned policy against the baseline using reproducible QEMU workloads

**Metrics:** mean scheduling latency, context switch overhead, CPU utilization,
starvation incidence.

### Benchmark workloads

Three deterministic workloads, each self-reporting scheduler telemetry via
`SYS_TELEMETRY` plus a checksum so runs are verifiable. Run from the shell
with `exec <name>.elf` (ELFs are packed into `disk.img` by the `disk` target).

#### bench_cpu -- CPU-bound
`user/bench_cpu.c`: 20M iterations of a deterministic integer-mix loop.
Measured under QEMU (TCG, no KVM), 4 runs:

| run | scheduler ticks consumed | checksum |
|-----|--------------------------|----------|
| 1 | 8  | -5780330217361705839 |
| 2 | 9  | -5780330217361705839 |
| 3 | 10 | -5780330217361705839 |
| 4 | 12 | -5780330217361705839 |

Identical checksums confirm determinism; tick counts vary ~10% run to run
(TCG timing nondeterminism).

#### bench_io -- I/O-bound
`user/bench_io.c`: 10 sequential passes over the 1 MiB `io_test.bin`
(generated by `scripts/gen_io_test.py`), 4 KiB reads, 10 MiB total.
Measured, 2 runs:

| run | ticks | syscalls | io_blocks | checksum |
|-----|-------|----------|-----------|----------|
| 1 | 155 | 2652 | 0 | -1022382063478803950 |
| 2 | 141 | 2602 | 0 | -1022382063478803950 |

`io_block_count` stays 0: the ATA driver uses polling PIO, so disk reads
never put the task to sleep. A blocking driver would make this workload's
scheduler signature far more interesting.

The benchmark exposed a diskfs heap leak: `diskfs_read_op` did a per-read
`kmalloc` for its sector bounce buffer while `kfree` is a no-op bump
allocator, exhausting the 8 MiB kernel heap after ~7 MiB of reads. Fixed by
reading whole sectors straight into the caller's buffer, using a 512-byte
stack bounce buffer only for partial head/tail sectors. (A real free-list
`kfree` remains future work.)

#### bench_mix -- mixed CPU+I/O
`user/bench_mix.c`: 10 rounds, each interleaving a 1M-iteration CPU burst
with a full 1 MiB file read (10M CPU iterations + 10 MiB I/O total).
Measured, 2 runs:

| run | ticks | syscalls | checksum |
|-----|-------|----------|----------|
| 1 | 177 | 2604 | 2932272040665676128 |
| 2 | 160 | 2604 | 2932272040665676128 |

Cross-checks against the pure benchmarks: ~5 ticks for the 10M CPU
iterations (bench_cpu: 20M ~= 10 ticks) plus ~150 ticks for the 10 MiB of
I/O (bench_io: ~141-155 ticks).

### Measured scheduler behavior (QEMU TCG)

- PIT programmed at 100 Hz; effective ~119 ticks/sec combined across two
  runnable tasks.
- `rr_pick_next` skips only DEAD/WAITING and never returns the current task,
  so every timer tick is a context switch when 2+ tasks are runnable.
- **The idle task is never deprioritized**: a full 256-record trace shows
  perfect idle/shell alternation (128 vs 127 transitions). CPU-bound work
  therefore shares the CPU ~50/50 with idle -- the first baseline
  inefficiency a learned policy should beat.

### Learned scheduling policy (policy 2)

A linear scoring policy trained offline via a trace-calibrated simulator
(`scripts/sim_search.py`), deployed as a third kernel scheduling policy
(`src/kernel/proc/policy_ml.c`, switchable with `policy 2`).

**Training:** random search + hill-climbing over 4-feature weight vectors,
optimizing mean normalized scheduling cost across four scenarios:
the three benchmarks plus a task-arrival scenario (new task joins at t=30)
that penalizes starvation. An earlier weight vector was rejected after it
was shown to starve newcomers ("prefer high total_ticks" overfits the
2-task case); the arrival scenario forced the search toward fair weights.

**Learned weights** (2^20 fixed point, integer-only in kernel):
- `total_c` +0.298: prefer less-run tasks (fairness, no starvation)
- `syscalls_c` -2.835: prefer high-syscall (I/O-bound/interactive) tasks
- `is_idle_c` +1.521: strongly avoid the idle task
- `total_others` +0.229: weak preference

**Evaluation** (QEMU, all three benchmarks):
| benchmark | RR ticks | learned ticks | learned preemptions |
|-----------|----------|---------------|---------------------|
| bench_cpu | 10 | 12 | 0 (12 under RR) |
| bench_io  | 144 | 116 | 0 |
| bench_mix | -- | 138 | 0 |

Under round-robin the worker is preempted to idle roughly every other tick
(~50% CPU share). Under the learned policy there is exactly one switch in
and one switch out -- the benchmark holds 100% CPU for its whole run.
The tick reduction (e.g. 144 -> 116 on bench_io) reflects eliminated
context-switch overhead, not less work (checksums identical).
- Two kernel fixes were required: `ml_pick_next` now scores the current
  RUNNING task as a keep-candidate (otherwise idle wins by default when the
  worker is RUNNING-not-READY), and `sched_tick` now accumulates
  `total_ticks` on every tick rather than only on switches (a
  never-preempted task previously reported 0 ticks).

### Test infrastructure

- `scripts/test_bench_cpu.sh` / `test_bench_io.sh` / `test_bench_mix.sh`:
  headless QEMU runs that type commands via the monitor, wait for the
  benchmark checksum on the serial log, then capture `trace`/`sched`.
- QEMU monitor quirk: `sendkey underscore` is invalid; type `_` as the
  `shift-minus` chord. Space is `spc`, `.` is `dot`.
- A wedged QEMU holds the disk image write lock; kill stale instances
  before re-running.

---

### Phase 2 — AI-Based Memory / Page Replacement

**Goal:** Replace static page eviction policy (e.g. LRU approximation via accessed bits)
with a learned policy that predicts which page is least likely to be referenced in the
near future, using access pattern history as input.

**Approach:**
- Instrument the VMM to record page access events (fault address, task, timestamp)
- Define a page replacement policy interface: given current page frame state, emit
  an eviction candidate
- Implement a baseline CLOCK/LRU approximation policy
- Train a model on recorded access traces
- Evaluate using page fault rate and working-set hit rate as primary metrics

**Metrics:** page fault rate, working set hit rate, eviction accuracy vs. OPT (Bélády's).

**Increment 1 (done, Oct 12 2026) — #PF handler wired:** vector 14 previously
had no IDT gate (a fault would triple-fault). Now `src/kernel/arch/exc_stubs.S`
provides a `pf_stub` that forwards the CPU-pushed error code to
`page_fault_handler()` in `src/kernel/arch/exceptions.c`, which reads CR2,
decodes the error-code bits (P/W/R, U/S, reserved, I/D) and prints them to
serial, then halts (no demand pager yet — increment 2). `exceptions_init()`
installs the gate (`idt_set_gate(14, pf_stub, 0x08, 0x8E)`). Shell command
`pf` deliberately faults from ring 3 (write to unmapped 0x50000000000);
`scripts/test_pf.sh` verifies the handler fires with
`cr2=0x0000050000000000 err=0x0000000000000006 (not-present write user)`
(2/2 QEMU runs passing).

**Increment 2 (done, Oct 12 2026) -- demand paging:** the #PF handler now demand-maps a 16-page user region at 0x20000000000 (2TB, clear of user ELFs, the user stack, and the inc.1 probe address). On a not-present ring-3 fault inside the region it allocates a zeroed PMM frame, maps it into the *faulting* address space (the interrupt does not switch CR3) with PRESENT|RW|USER, prints `[pf] demand-mapped page ...`, and returns -- pf_stub's iretq re-executes the faulting instruction, which now succeeds. Faults outside the region (or protection/supervisor faults) still halt, unchanged from increment 1. `user/demand_test.c` writes a distinct pattern to 4 pages (one #PF each) and verifies the data back; `scripts/test_demand.sh` checks for exactly 4 demand-map lines, the expected page addresses, `[demand] PASS`, and no halt. Verified 1/1 QEMU run; `scripts/test_pf.sh` still passes (out-of-region fault halts with identical output).

**Increment 3 (done, Oct 12 2026) -- fault telemetry:** every #PF is recorded in a 256-entry kernel ring (`struct pf_record`: tick, CR2, error code, faulting pid, resolved flag; 32 bytes each) before the handler decides to demand-map or halt. New `SYS_PF_DRAIN` syscall (16) drains records to userspace; shell `pftrace` command prints them (`[i] tick=... cr2=0x... err=... pid=... resolved=...`). `scripts/test_pftrace.sh` runs demand_test then checks pftrace shows 4 records, all resolved=1, with the expected CR2 addresses and non-zero pids. This is the event stream a learned eviction policy will train on.

**Increment 4 (done, Oct 12 2026) -- eviction policy interface + FIFO baseline:** the demand region now has a 4-frame quota tracked in a kernel frame table (`struct demand_frame`: virt, phys, sequence, owning CR3). When a fault needs a frame and the quota is exhausted, the active `struct pf_policy` picks a victim via `pick_victim()`; the victim is unmapped (`vmm_unmap_page` + TLB invalidate) and its frame reused. The baseline `pf_policy_fifo` evicts the longest-resident frame. `user/evict_test.c` touches all 16 pages sequentially (16 faults, 12 evictions) then re-touches page 0 (1 more fault + eviction); `scripts/test_evict.sh` verifies 17 demand-maps, 13 evictions in FIFO order, and no halt. All earlier tests (pf, demand, pftrace) still pass.

---

### Phase 3 — AI-Assisted I/O Scheduling

**Goal:** Replace static I/O request ordering (FCFS or deadline) with a learned policy
that prioritizes I/O requests based on predicted latency sensitivity and workload type.

**Approach:**
- Instrument the ATA/disk layer to record request arrival time, queue depth, and
  service time per request
- Define an I/O policy interface: given a pending request queue, emit a reordered
  service sequence
- Implement a baseline FCFS and simple deadline policy
- Evaluate a learned reordering policy against both baselines

**Metrics:** mean I/O latency, throughput (sectors/sec), queue starvation rate.

---

## Benchmarking Philosophy

All learned policies are evaluated against deterministic baselines on identical
workloads running inside QEMU. Reproducibility is a design requirement:
the same workload run twice must produce comparable telemetry.

Primary metrics across all subsystems:

| Metric                    | Subsystem        |
|---------------------------|------------------|
| Mean scheduling latency   | CPU scheduler    |
| Context switch overhead   | CPU scheduler    |
| Page fault rate           | Memory manager   |
| Cache / working-set hits  | Memory manager   |
| Mean I/O latency          | I/O scheduler    |
| Disk throughput           | I/O scheduler    |

Comparison targets are modeled on documented Linux behavior (CFS scheduling,
CLOCK page replacement, CFQ/deadline I/O) rather than on Linux kernel code directly.

---

## Non-Goals

This project explicitly does not aim to:

- Replace or compete with Linux, Windows, or any production OS
- Provide a general-purpose desktop or server environment
- Implement a full POSIX-compatible interface
- Achieve production-level stability, security, or hardware support

It is a research and experimentation kernel. Stability is valued insofar as it enables
reproducible benchmarks; beyond that, simplicity of instrumentation takes priority.

---

## Build & Run

### Requirements

- GCC cross-compiler: `x86_64-elf-gcc`
- GNU binutils for `x86_64-elf`
- QEMU: `qemu-system-x86_64`
- `grub-mkrescue`, `xorriso`
- Python 3 (for disk image builder)
- `make`

### Quick start (WSL/Ubuntu)

```bash
# Inside WSL Ubuntu
sudo apt update
sudo apt install build-essential bison flex libgmp3-dev libmpc-dev libmpfr-dev \
                 texinfo libisl-dev qemu-system-x86 grub-pc-bin xorriso python3

# Cross-compiler (one-time build, ~10 min)
export PREFIX="$HOME/opt/cross"
export TARGET=x86_64-elf
export PATH="$PREFIX/bin:$PATH"
# ... (see scripts/build_toolchain.sh for full steps)
```

### Build and boot

```bash
cd /mnt/c/Users/<you>/Documents/Projects/MyOS
make iso    # builds kernel ELF + ISO + disk image
make run    # boots under QEMU
```

`make run` launches QEMU with a VGA window and serial debug output on stdout.
Serial output includes structured `[serial]` boot messages useful for automated testing.

### Headless testing

```bash
bash scripts/test_headless.sh
```

Boots QEMU without a display, injects keystrokes via the QEMU monitor, and captures
serial output for automated verification.

---

## Repository Layout

```
src/kernel/
  arch/       — GDT, IDT, IRQ, PIC, PIT, port I/O
  mem/        — PMM, VMM (4-level paging), kernel heap
  proc/       — scheduler, ELF loader, context switch
  sys/        — syscall dispatch
  drivers/    — VGA, keyboard, RTC, ATA
  fs/         — VFS, ramfs, MyFS disk backend
src/lib/      — user-space syscall stubs, malloc
user/         — shell (hello.c) and user programs
disk_files/   — files packed into the MyFS disk image at build time
scripts/      — mkfs_myfs.py (disk builder), test_headless.sh
boot/         — GRUB config, linker scripts
```

---

## License

Research prototype. No warranty. See LICENSE for details.

