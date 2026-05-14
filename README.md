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

