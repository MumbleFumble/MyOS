#pragma once

#include <stdint.h>

/* Maximum number of tasks the scheduler can hold. */
#define MAX_TASKS    32

/* Kernel stack size per task (8 KiB). */
#define TASK_STACK_SIZE  8192

/* Exit code recorded for a task killed by the fault path
 * (Phase 17 inc.2): a user-mode CPU fault outside the
 * demand region, or a user-mode #DE/#DB/#UD/#SS/#GP.
 * Distinct from every voluntary exit code in the tree
 * (existing programs exit 0 or 1). Mirrored for user
 * programs in src/lib/syscall.h. */
#define EXIT_FAULT  4000

/* Signal-death status band (Phase 18 inc.3): a task
 * terminated by SYS_KILL with signal sig collects
 * EXIT_SIGNAL_BASE + sig, so a waiter can tell a
 * signal death from a voluntary exit and from
 * EXIT_FAULT, and can tell WHICH signal. Replaces
 * inc.2's flat EXIT_KILLED 4001, which no path
 * produces any more (SYS_KILL always carries a
 * signal now). Like EXIT_FAULT, the band is spoofable
 * by a program exiting with the same value.
 * Mirrored for user programs in src/lib/syscall.h. */
#define EXIT_SIGNAL_BASE 4096

/* Maximum open file descriptors per task. fds 0/1/2 are stdin/stdout/stderr. */
#define MAX_FDS  16

/* struct file + struct fd_ops live in fd.h (Phase 14 inc.1):
 * every fd, console included, dispatches through a per-fd ops
 * table, and ops == NULL is the free-slot test. */
#include "fd.h"

typedef enum {
    TASK_READY   = 0,
    TASK_RUNNING = 1,
    TASK_DEAD    = 2,
    TASK_WAITING = 3,   /* blocked in sched_wait_pid */
    TASK_BLOCKED = 4,   /* blocked on I/O (sched_block_current_io) */
} task_state_t;

/*
 * Saved kernel-mode register context.
 * context_switch.S saves/restores exactly these fields on the task's
 * kernel stack, then returns into the new task via ret.
 *
 * Layout must match context_switch.S exactly.
 */
struct task_regs {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rip;   /* pushed last by context_switch as the "return address" */
};

struct task {
    uint64_t      rsp;          /* saved kernel stack pointer */
    uint8_t      *stack;        /* base of the kmalloc'd kernel stack */
    void        (*entry)(void); /* entry function, read by trampoline */
    task_state_t  state;
    uint32_t      pid;
    const char   *name;
    uint64_t      cr3;          /* physical address of PML4 (0 = use current) */
    /* User-space heap (managed by SYS_SBRK) */
    uint64_t      heap_base;    /* lowest heap virtual address */
    uint64_t      heap_end;     /* current break (next free byte) */
    /* Process relationships */
    uint32_t      parent_pid;   /* pid of task that spawned this one, 0 = none */
    uint32_t      pgid;         /* process group (Phase 18 inc.4): inherited at
                                 * spawn; 0 = no group (kernel tasks only) */
    int32_t       exit_code;    /* exit status, valid when state == TASK_DEAD */
    uint32_t      wait_pid;     /* pid we are waiting on (0 = any child), TASK_WAITING only */
    /* Open file descriptor table: slots point at shared
     * descriptions (fd.h); NULL = free. fd_pinned bit i set
     * means close() on fd i fails (the fresh console fds). */
    struct file  *fds[MAX_FDS];
    uint16_t      fd_pinned;
    /* ---- Telemetry (written by scheduler, read by SYS_TELEMETRY) ---- */
    uint64_t      total_ticks;          /* cumulative ticks this task has run */
    uint64_t      last_scheduled_tick;  /* tick when this task was last made RUNNING */
    uint64_t      wait_ticks;           /* cumulative ticks spent in TASK_WAITING */
    uint64_t      wait_start_tick;      /* tick when this task entered TASK_WAITING/BLOCKED */
    uint64_t      io_deadline_tick;     /* TASK_BLOCKED only: wake by this tick (0 = none) */
    uint64_t      syscall_count;        /* number of syscalls dispatched */
    uint64_t      io_block_count;       /* times blocked on I/O (incremented by fs layer) */
};

/* -----------------------------------------------------------------------
 * Pluggable scheduling policy interface.
 *
 * A policy module fills in this struct and passes it to sched_set_policy().
 * All callbacks receive the current tick for time-aware decisions.
 * ----------------------------------------------------------------------- */
struct sched_policy {
    const char *name;

    /* Called once when the policy is installed. */
    void (*init)(void);

    /*
     * Pick the index (into the internal task table) of the next task to run.
     * 'current_idx' is the index of the task currently running.
     * 'tasks'       is a read-only view of the task array.
     * 'count'       is the number of slots (some may be TASK_DEAD).
     * 'tick'        is the current timer tick.
     * Must return a valid index of a TASK_READY task, or current_idx if
     * nothing else is runnable.
     */
    uint32_t (*pick_next)(uint32_t current_idx,
                          const struct task *tasks,
                          uint32_t count,
                          uint64_t tick);

    /* Notified when a task is added / becomes runnable / exits. */
    void (*task_added)  (uint32_t idx, const struct task *tasks, uint32_t count);
    void (*task_removed)(uint32_t idx, const struct task *tasks, uint32_t count);
};

/*
 * Replace the active scheduling policy at runtime.
 * The previous policy is not freed — caller manages lifetime.
 * Pass NULL to revert to the built-in round-robin policy.
 */
void sched_set_policy(struct sched_policy *p);

/*
 * Initialise the scheduler and register the idle task (the current
 * execution context — kernel_main's loop).
 */
void sched_init(void);

/*
 * Create a new kernel task running func().
 * Returns the new task's pid, or 0 on failure.
 */
uint32_t task_create(const char *name, void (*func)(void));

/*
 * Called from the timer IRQ every tick.
 * Picks the next READY task and switches to it.
 * 'tick' is the current timer_ticks() value — used for telemetry and
 * policy time-slice calculations.
 */
void sched_tick(uint64_t tick);

/* Called by sys_exit: mark current task dead and switch away immediately. */
void sched_current_exit(void);

/* Terminate the task with the given pid on behalf of
 * another task (Phase 18 inc.2, SYS_KILL; inc.3 added
 * the signal): marks it DEAD with EXIT_SIGNAL_BASE +
 * sig, wakes its waiters and tears down its fds, all
 * in one cli section. Returns 0, or -1 when the pid
 * does not exist, is already DEAD, or names a kernel
 * task (cr3 == 0). Never handles the caller itself.
 * The caller (sys_kill) has already validated sig
 * against the signal vocabulary. */
int sched_kill_task(uint32_t pid, int sig);

/* Group form of the kill (Phase 18 inc.4): terminate
 * every live user task whose pgid matches, in one cli
 * section, same per-task discipline as sched_kill_task.
 * Returns 0 if at least one task was signalled, -1 if
 * the group has no live user members. The caller may
 * itself be among the victims; sys_kill finishes that
 * case through sched_current_exit. */
int sched_kill_group(uint32_t pgid, int sig);

/* Process-group identity (Phase 18 inc.4).
 * sched_getpgid: pid 0 names the caller; a DEAD task
 * still reports its last pgid (slots persist); an
 * unknown pid returns -1; kernel tasks report 0.
 * sched_setpgid: pid 0 names the caller, pgid 0 means
 * "the target's own pid" (become a group leader);
 * refuses (-1, nothing changed) unknown or DEAD
 * targets, kernel tasks, and a target group that has
 * no live user member and is not the target's own pid. */
int64_t sched_getpgid(uint32_t pid);
int64_t sched_setpgid(uint32_t pid, uint32_t pgid);

/* Foreground group + Ctrl-C delivery (Phase 18 inc.5).
 * The foreground record (fg pgid + the pid that set
 * it) is written by SYS_SETFG and read by the
 * keyboard path. sched_note_ctrlc is the ONLY thing
 * the keyboard IRQ calls: it snapshots the record
 * into a pending slot and wakes the delivery task --
 * no kill or teardown work in IRQ context. The
 * delivery task (sched_sigint_task, a kernel task)
 * performs the group kill from task context.
 * Delivery spares the recorded setter: at an idle
 * prompt the foreground group is the shell's own and
 * its only member is the shell, so a prompt-level
 * Ctrl-C signals nobody. */
void sched_fg_set(uint32_t pgid, uint32_t setter_pid);
void sched_note_ctrlc(void);
void sched_sigint_task(void);
int sched_kill_group_excl(uint32_t pgid, int sig, uint32_t excl_pid);

/*
 * Create a ring-3 task from a pre-loaded ELF result.
 * Sets up a kernel stack with an iretq frame targeting the ELF entry point
 * in the given address space.  The task will drop to ring 3 when first scheduled.
 */
uint32_t user_task_create(const char *name, uint64_t cr3, uint64_t entry, uint64_t ustack);

/* Returns a pointer to the currently running task (never NULL after sched_init). */
struct task *sched_current_task(void);

/* Find a task by pid. Returns NULL if not found. */
struct task *sched_find_by_pid(uint32_t pid);

/*
 * Block the calling task until any direct child with the given pid exits.
 * Pass pid=0 to wait for any child.
 * Returns the child's exit code, or -1 if no matching child exists.
 */
int32_t sched_wait_pid(uint32_t pid);

/*
 * Block the calling task until sched_wake_io() is called on it (e.g. from
 * an IRQ handler) or 'deadline_tick' passes, whichever comes first.
 * Increments the task's io_block_count. Returns 1 if the task blocked,
 * 0 if blocking was refused (the idle task never blocks) and the caller
 * should poll instead.
 */
int sched_block_current_io(uint64_t deadline_tick);

/*
 * As sched_block_current_io, but does not count the block as I/O: the
 * generic sleep behind the I/O wrapper, for kernel-internal waits
 * (locks, work queues). Same contract: 1 if blocked, 0 if refused
 * (idle task).
 */
int sched_block_current(uint64_t deadline_tick);

/* Wake a TASK_BLOCKED task. IRQ-safe: plain stores only, no switching. */
void sched_wake_io(struct task *t);

/* Set by IRQ handlers that woke a task and want it run promptly. The
 * idle loop polls this flag and calls sched_tick from its top level:
 * sched_tick is not reentrant, so IRQ handlers must never call it
 * directly (nested switches corrupt saved stack pointers). Any
 * sched_tick run clears the flag. */
extern volatile int sched_resched_pending;
void sched_request_resched(void);

/* Low-level context switch (context_switch.S). */
void context_switch(uint64_t *old_rsp, uint64_t new_rsp);

/* -----------------------------------------------------------------------
 * Telemetry snapshot — one per task, written by SYS_TELEMETRY.
 * Layout is stable; userspace mirrors it in lib/syscall.h.
 * ----------------------------------------------------------------------- */
typedef struct {
    uint32_t pid;
    uint8_t  state;      /* task_state_t cast to uint8 */
    uint8_t  _pad[3];
    char     name[16];   /* truncated to 15 chars + NUL */
    uint64_t total_ticks;
    uint64_t wait_ticks;
    uint64_t syscall_count;
    uint64_t io_block_count;
} task_stat_t;           /* 56 bytes */

/*
 * Fill 'buf' with up to 'max' task_stat_t snapshots.
 * Returns the number of active (non-DEAD) tasks copied.
 */
int sched_get_stats(task_stat_t *buf, int max);

/* -----------------------------------------------------------------------
 * Switch-trace telemetry -- per-context-switch records for offline ML
 * training (Phase 1 of the research roadmap).
 *
 * Every real context switch appends one record capturing the tick, the
 * previous and next task indices, and a snapshot of the full task table
 * as the policy saw it. Userspace drains the ring with SYS_TELEMETRY_DRAIN
 * and trains models offline on the (run-queue state -> next_task) pairs.
 *
 * Layout is stable; userspace mirrors it in lib/syscall.h.
 * ----------------------------------------------------------------------- */

#define TRACE_MAX_TASKS  32
#define TRACE_RING_SIZE  256

struct trace_task_snapshot {
    uint32_t pid;
    uint8_t  state;      /* task_state_t cast to uint8 */
    uint8_t  _pad[3];
    uint64_t total_ticks;
    uint64_t wait_ticks;
    uint64_t syscall_count;
    uint64_t io_block_count;
};                       /* 40 bytes */

struct switch_record {
    uint64_t tick;
    uint32_t prev_idx;   /* task table index of outgoing task */
    uint32_t next_idx;   /* task table index of incoming task */
    uint32_t task_count; /* number of valid entries in tasks[] */
    uint32_t _pad;
    struct trace_task_snapshot tasks[TRACE_MAX_TASKS];
};                       /* 1304 bytes */

/*
 * Drain up to 'max' records (oldest first) from the ring into 'buf'
 * and clear the ring. Returns the number of records copied.
 * Safe to call from syscall context; briefly disables interrupts.
 */
int sched_trace_drain(struct switch_record *buf, int max);

/* ABI stability: userspace mirrors these layouts exactly. */
_Static_assert(sizeof(struct trace_task_snapshot) == 40, "trace_task_snapshot layout changed");
_Static_assert(sizeof(struct switch_record) == 1304, "switch_record layout changed");
