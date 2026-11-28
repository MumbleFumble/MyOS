#include "sched.h"
#include "../mem/kheap.h"
#include "../mem/vmm.h"
#include "../arch/gdt.h"
#include "../arch/timer.h"

/* -----------------------------------------------------------------------
 * Task table
 * ----------------------------------------------------------------------- */

static struct task tasks[MAX_TASKS];
static uint32_t   task_count = 0;
static uint32_t   current    = 0;   /* index of currently running task */
static uint32_t   next_pid   = 1;

/* -----------------------------------------------------------------------
 * Switch-trace ring buffer.
 *
 * Written only by sched_tick() (IRQ or syscall context, allocation-free)
 * and drained by sched_trace_drain(). When full, the oldest record is
 * overwritten. All writes are plain stores; the drain side disables
 * interrupts while copying.
 * ----------------------------------------------------------------------- */
static struct switch_record trace_ring[TRACE_RING_SIZE];
static uint32_t trace_head  = 0;  /* index of oldest valid record */
static uint32_t trace_count = 0;  /* number of valid records */

/* -----------------------------------------------------------------------
 * Built-in round-robin policy
 * ----------------------------------------------------------------------- */

static uint32_t rr_pick_next(uint32_t cur, const struct task *t,
                              uint32_t count, uint64_t tick)
{
    (void)tick;
    uint32_t next  = (cur + 1) % count;
    uint32_t tries = 0;
    while ((t[next].state == TASK_DEAD || t[next].state == TASK_WAITING
            || t[next].state == TASK_BLOCKED)
           && tries < count) {
        next = (next + 1) % count;
        tries++;
    }
    return next;
}

static struct sched_policy rr_policy = {
    .name         = "round-robin",
    .init         = 0,
    .pick_next    = rr_pick_next,
    .task_added   = 0,
    .task_removed = 0,
};

static struct sched_policy *active_policy = &rr_policy;

void sched_set_policy(struct sched_policy *p)
{
    active_policy = p ? p : &rr_policy;
    if (active_policy->init)
        active_policy->init();
}

int sched_get_stats(task_stat_t *buf, int max)
{
    int n = 0;
    for (uint32_t i = 0; i < task_count && n < max; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_DEAD)
            continue;
        buf[n].pid            = t->pid;
        buf[n].state          = (uint8_t)t->state;
        buf[n]._pad[0]        = 0;
        buf[n]._pad[1]        = 0;
        buf[n]._pad[2]        = 0;
        buf[n].total_ticks    = t->total_ticks;
        buf[n].wait_ticks     = t->wait_ticks;
        buf[n].syscall_count  = t->syscall_count;
        buf[n].io_block_count = t->io_block_count;
        /* Copy name, truncated to 15 chars */
        int j = 0;
        while (j < 15 && t->name && t->name[j]) {
            buf[n].name[j] = t->name[j];
            j++;
        }
        buf[n].name[j] = '\0';
        n++;
    }
    return n;
}

/*
 * Drain the switch-trace ring: copy up to 'max' records (oldest first) into
 * 'buf' and clear the ring. Returns the number of records copied.
 *
 * Interrupts are disabled during the copy so a timer tick cannot interleave
 * a record write mid-copy. Called from syscall context (IF=1 on entry via
 * the int 0x80 trap gate), so cli/sti pairing is safe here.
 */
int sched_trace_drain(struct switch_record *buf, int max)
{
    if (!buf || max <= 0)
        return 0;

    __asm__ volatile("cli");
    uint32_t n = trace_count < (uint32_t)max ? trace_count : (uint32_t)max;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = (trace_head + i) % TRACE_RING_SIZE;
        buf[i] = trace_ring[slot];
    }
    trace_head   = (trace_head + n) % TRACE_RING_SIZE;
    trace_count -= n;
    __asm__ volatile("sti");

    return (int)n;
}

/* Forward declaration */
static void task_entry_trampoline(void);

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/* The boot address space's CR3, captured at sched_init.
 * Kernel tasks share it; user tasks always run on a
 * fresh PML4 from the loader, so cr3 == kernel_cr3 is
 * the reliable kernel-task test (Phase 18 inc.4: the
 * inc.2 guard compared against 0, which no task ever
 * has -- tasks[0] included -- so the documented
 * kernel-task refusal never actually fired until this
 * discriminator replaced it). */
static uint64_t kernel_cr3;
static int task_is_kernel(const struct task *t) { return t->cr3 == kernel_cr3; }

void sched_init(void)
{
    /* Task 0 = idle / kernel_main — it's already running.
     * We don't need to set up a stack for it; sched_tick will save its
     * rsp into tasks[0].rsp on the first switch away from it. */
    tasks[0].rsp   = 0;            /* filled in by context_switch on first preempt */
    tasks[0].stack = 0;            /* not kmalloc'd — uses the boot stack */
    tasks[0].state = TASK_RUNNING;
    tasks[0].pid   = next_pid++;
    tasks[0].name  = "idle";
    fd_init_std(&tasks[0]);
    tasks[0].cr3   = vmm_current_cr3();
    kernel_cr3     = tasks[0].cr3;
    tasks[0].pgid  = 0;
    task_count     = 1;
    current        = 0;
}

uint32_t task_create(const char *name, void (*func)(void))
{
    if (task_count >= MAX_TASKS)
        return 0;

    struct task *t = &tasks[task_count];

    t->stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!t->stack)
        return 0;

    /* Zero the stack */
    for (uint32_t i = 0; i < TASK_STACK_SIZE; i++)
        t->stack[i] = 0;

    fd_init_std(t);
    t->state = TASK_READY;
    t->pid   = next_pid++;
    t->name  = name;
    t->entry = func;
    t->cr3   = vmm_current_cr3();   /* kernel tasks share the boot address space */
    t->pgid  = 0;                   /* kernel tasks belong to no group */
    t->parent_pid = 0;
    t->exit_code  = 0;
    t->wait_pid   = 0;

    /* Set up initial stack frame. */
    uint8_t *stack_top = t->stack + TASK_STACK_SIZE;
    stack_top = (uint8_t *)((uint64_t)stack_top & ~0xFUL);

    /* context_switch.S pops: rbp, rbx, r12, r13, r14, r15, then rets to rip. */
    stack_top -= 8; *((uint64_t *)stack_top) = (uint64_t)task_entry_trampoline; /* rip */
    stack_top -= 8; *((uint64_t *)stack_top) = 0;                               /* r15 */
    stack_top -= 8; *((uint64_t *)stack_top) = 0;                               /* r14 */
    stack_top -= 8; *((uint64_t *)stack_top) = 0;                               /* r13 */
    stack_top -= 8; *((uint64_t *)stack_top) = 0;                               /* r12 */
    stack_top -= 8; *((uint64_t *)stack_top) = 0;                               /* rbx */
    stack_top -= 8; *((uint64_t *)stack_top) = 0;                               /* rbp <- rsp */

    t->rsp = (uint64_t)stack_top;

    task_count++;

    if (active_policy->task_added)
        active_policy->task_added(task_count - 1, tasks, task_count);

    return t->pid;
}

/*
 * sched_tick() is not reentrant, and every voluntary call site used to
 * run it with interrupts ON (sti immediately before the call): a timer
 * IRQ landing inside the call interleaves a second switch sequence and
 * corrupts saved stack pointers. The failure was latent for months
 * because the window needs a nested pick that switches to a *specific*
 * task; Phase 4's prefetch worker (blocking every few ticks) made it
 * fire within seconds. Rule: sched_tick is only ever called with IF=0
 * (IRQ gates already guarantee this; voluntary callers use this
 * wrapper, which also re-enables interrupts for the resumed task).
 */
static void sched_tick_cli(void)
{
    __asm__ volatile("cli");
    sched_tick((uint64_t)timer_ticks());
    __asm__ volatile("sti");
}

/* Wake every task waiting (TASK_WAITING) on pid, with
 * the wait-time accounting. Caller holds IRQs off.
 * Shared by sched_current_exit and sched_kill_task
 * (Phase 18 inc.2). */
static void sched_wake_waiters(uint32_t pid)
{
    uint64_t now = (uint64_t)timer_ticks();
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_WAITING &&
            (tasks[i].wait_pid == 0 || tasks[i].wait_pid == pid))
        {
            /* Accumulate the time this task spent blocked.
             * wait_start_tick was recorded when the task
             * entered TASK_WAITING. */
            tasks[i].wait_ticks += now - tasks[i].wait_start_tick;
            tasks[i].state = TASK_READY;
        }
    }
}

void sched_current_exit(void)
{
    /* IRQs off for the whole mark-and-wake sequence: called
     * from sys_exit with IF=1 (trap gate), a timer IRQ in
     * this window could switch the dying task out before
     * its waiters are woken, stranding them forever. The
     * fault path (Phase 17 inc.2) already runs with IF=0.
     * sched_tick_cli's sti restores IF for the next task. */
    __asm__ volatile("cli");
    tasks[current].state     = TASK_DEAD;
    /* exit_code is NOT reset here: the caller (sys_exit, or
     * the fault path with EXIT_FAULT) sets it before calling
     * us, and waiters collect exactly that value. (Before
     * Phase 17 inc.2 this line zeroed it, so SYS_WAIT could
     * never deliver a nonzero code.) Task creation
     * initialises exit_code to 0 for tasks that never set
     * one. */

    /* Wake any task waiting for this pid */
    sched_wake_waiters(tasks[current].pid);

    sched_tick_cli();
    for (;;) __asm__ volatile("hlt");
}

/* Phase 18 inc.2: terminate the task with the given pid
 * on behalf of another task (SYS_KILL). The whole
 * operation -- find, mark DEAD, wake waiters, fd
 * teardown -- runs in ONE cli section, so no third party
 * can observe or interleave a half-killed task, EOF on
 * the victim's pipes is atomic with its waiters' wakeup,
 * and a second kill finds the task already DEAD. The
 * backend closes fd_teardown_task runs are non-blocking
 * bookkeeping in this kernel (desc_put restores the
 * saved flags, here IF=0), so the window stays short.
 *
 * Never-resume is by state, not by flag: every path that
 * can make a task runnable is source-state-guarded
 * (sched_wake_io and the IRQ deadline scan only touch
 * TASK_BLOCKED, the exit wake only TASK_WAITING,
 * pick_next only READY/RUNNING, sched_tick refuses to
 * re-mark a DEAD task READY), so a killed task's
 * abandoned syscall frame is never resumed and its
 * pending io_deadline_tick is never consulted again.
 * Driver state a task killed mid-wait abandons (an
 * in-flight ATA command's lock, a TCP wait) is the same
 * exposure the fault-kill path has always had; the wait
 * itself is deadline-bounded where it matters.
 *
 * Returns 0 on success, -1 when the pid does not exist,
 * the task is already DEAD, or the target is a kernel
 * task (idle, net, ... -- kernel tasks are unkillable;
 * the test is task_is_kernel, see sched_init). The
 * caller itself is NOT handled here: sys_kill routes
 * self-kill through the exit path. There is no
 * permission model: any task may kill any user task
 * (this kernel's tradition). */
/* The per-task kill core: caller holds IRQs off and has
 * already validated t (a live user task). Shared by
 * sched_kill_task and sched_kill_group (inc.4). */
static void kill_mark(struct task *t, int sig)
{
    t->exit_code = EXIT_SIGNAL_BASE + sig;
    t->state     = TASK_DEAD;
    sched_wake_waiters(t->pid);
    fd_teardown_task(t);
}

int sched_kill_task(uint32_t pid, int sig)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    __asm__ volatile("cli");
    struct task *t = sched_find_by_pid(pid);
    if (!t || t->state == TASK_DEAD || task_is_kernel(t)) {
        if (rflags & 0x200) __asm__ volatile("sti");
        return -1;
    }
    kill_mark(t, sig);
    if (rflags & 0x200) __asm__ volatile("sti");
    return 0;
}

/* Phase 18 inc.4: terminate every live user task in a
 * process group, in ONE cli section with the same
 * per-task discipline as sched_kill_task (the closes
 * fd_teardown_task runs are non-blocking, so the
 * section stays short). Iteration order is table
 * order; a member woken by an earlier member's death
 * is still marked when its turn comes, and a waiter
 * outside the group collects normally. The CALLER is
 * not spared if it belongs to the group (POSIX): it is
 * marked like any member, and sys_kill finishes the
 * call through sched_current_exit instead of returning
 * into a dead task. Returns 0 when at least one task
 * was signalled, -1 when the group has no live user
 * members (empty or nonexistent group; group 0 names
 * the kernel tasks, which are never signalled). */
static int group_kill(uint32_t pgid, int sig, uint32_t excl_pid)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    __asm__ volatile("cli");
    int signalled = 0;
    for (uint32_t i = 0; i < task_count; i++) {
        struct task *t = &tasks[i];
        if (t->pgid == pgid && t->state != TASK_DEAD &&
            t->pid != excl_pid && !task_is_kernel(t)) {
            kill_mark(t, sig);
            signalled = 1;
        }
    }
    if (rflags & 0x200) __asm__ volatile("sti");
    return signalled ? 0 : -1;
}

int sched_kill_group(uint32_t pgid, int sig)
{
    return group_kill(pgid, sig, 0);
}

/* As sched_kill_group, but one pid is spared. Used
 * ONLY by the Ctrl-C delivery path (inc.5), never by
 * SYS_KILL: the foreground record's setter (the
 * shell) must survive signalling the group it is
 * itself waiting on -- during an exec the child
 * shares the shell's group, and at the idle prompt
 * the shell is the group's only member. */
int sched_kill_group_excl(uint32_t pgid, int sig, uint32_t excl_pid)
{
    return group_kill(pgid, sig, excl_pid);
}

/* -----------------------------------------------------------------------
 * Foreground group record + deferred Ctrl-C delivery
 * (Phase 18 inc.5). fg_pgid starts 0 (no foreground):
 * a Ctrl-C before anyone sets the record delivers to
 * group 0, which has no user members -- a no-op.
 * ----------------------------------------------------------------------- */
static volatile uint32_t fg_pgid   = 0;
static volatile uint32_t fg_setter = 0;

void sched_fg_set(uint32_t pgid, uint32_t setter_pid)
{
    fg_pgid   = pgid;
    fg_setter = setter_pid;
}

static volatile int      sigint_pending = 0;
static volatile uint32_t sigint_pgid    = 0;
static volatile uint32_t sigint_setter  = 0;
static struct task      *sigint_task    = 0;

/* Keyboard IRQ context: record ONLY. Snapshotting
 * the foreground record here means a later SYS_SETFG
 * cannot redirect a Ctrl-C that was already pressed.
 * The wake mirrors the e1000 RX pattern. */
void sched_note_ctrlc(void)
{
    sigint_pgid   = fg_pgid;
    sigint_setter = fg_setter;
    sigint_pending = 1;
    if (sigint_task && sigint_task->state == TASK_BLOCKED)
        sched_wake_io(sigint_task);
}

/* The delivery context: a kernel task, never an IRQ.
 * Blocks on the I/O wait with a 50-tick fallback poll
 * (the net RX task's pattern), so a lost wake delays
 * a delivery by at most half a second. The result of
 * the kill is deliberately ignored: delivery to an
 * empty or stale group is a no-op, not an error. */
void sched_sigint_task(void)
{
    sigint_task = sched_current_task();
    for (;;) {
        if (sigint_pending) {
            uint32_t pgid, setter;
            __asm__ volatile("cli");
            pgid   = sigint_pgid;
            setter = sigint_setter;
            sigint_pending = 0;
            __asm__ volatile("sti");
            sched_kill_group_excl(pgid, 2 /* SIGINT */, setter);
        } else {
            sched_block_current_io((uint64_t)timer_ticks() + 50);
        }
    }
}

int64_t sched_getpgid(uint32_t pid)
{
    if (pid == 0) {
        struct task *c = sched_current_task();
        return c ? (int64_t)c->pgid : -1;
    }
    struct task *t = sched_find_by_pid(pid);
    return t ? (int64_t)t->pgid : -1;
}

/* The check-and-set runs under cli so a concurrent
 * group kill observes the pgid either before or after
 * the move, never a half-moved state. */
int64_t sched_setpgid(uint32_t pid, uint32_t pgid)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    __asm__ volatile("cli");
    struct task *t = pid ? sched_find_by_pid(pid)
                         : sched_current_task();
    int64_t rc = -1;
    if (t && t->state != TASK_DEAD && !task_is_kernel(t)) {
        uint32_t ng = pgid ? pgid : t->pid;
        if (ng == t->pgid) {
            rc = 0;                     /* already there */
        } else if (ng == t->pid) {
            t->pgid = ng; rc = 0;       /* become leader */
        } else {
            for (uint32_t i = 0; i < task_count; i++) {
                struct task *u = &tasks[i];
                if (u != t && u->pgid == ng &&
                    u->state != TASK_DEAD && !task_is_kernel(u)) {
                    t->pgid = ng; rc = 0; break;
                }
            }
        }
    }
    if (rflags & 0x200) __asm__ volatile("sti");
    return rc;
}

struct task *sched_current_task(void)
{
    return &tasks[current];
}

struct task *sched_find_by_pid(uint32_t pid)
{
    for (uint32_t i = 0; i < task_count; i++)
        if (tasks[i].pid == pid) return &tasks[i];
    return 0;
}

int32_t sched_wait_pid(uint32_t pid)
{
    /* Check if a matching dead child already exists */
check:
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].state != TASK_DEAD) continue;
        if (tasks[i].parent_pid != tasks[current].pid) continue;
        if (pid != 0 && tasks[i].pid != pid) continue;
        /* Found a dead child — collect its exit code */
        return tasks[i].exit_code;
    }
    /* No dead child yet — block until one finishes. The
     * re-check and the transition to TASK_WAITING run with
     * IRQs off: sched_current_exit wakes only tasks already
     * in TASK_WAITING, so a child exiting between the check
     * and the state store would lose its wakeup and strand
     * this waiter (Phase 17 inc.2; waiting is now on the
     * shell's critical path via the pf probe). */
    __asm__ volatile("cli");
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].state != TASK_DEAD) continue;
        if (tasks[i].parent_pid != tasks[current].pid) continue;
        if (pid != 0 && tasks[i].pid != pid) continue;
        __asm__ volatile("sti");
        return tasks[i].exit_code;
    }
    int has_child = 0;
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].parent_pid != tasks[current].pid) continue;
        if (pid != 0 && tasks[i].pid != pid) continue;
        if (tasks[i].state != TASK_DEAD) { has_child = 1; break; }
    }
    if (!has_child) { __asm__ volatile("sti"); return -1; }

    tasks[current].state    = TASK_WAITING;
    tasks[current].wait_pid = pid;
    sched_tick_cli();
    /* After being woken we are RUNNING again; re-check */
    goto check;
}

int sched_block_current(uint64_t deadline_tick)
{
    if (current == 0)
        return 0;   /* the idle task must never block */

    tasks[current].state            = TASK_BLOCKED;
    tasks[current].io_deadline_tick = deadline_tick;
    tasks[current].wait_start_tick  = (uint64_t)timer_ticks();
    sched_tick_cli();
    /* Resumed: woken by sched_wake_io() or by the deadline scan. */
    return 1;
}

int sched_block_current_io(uint64_t deadline_tick)
{
    if (current == 0)
        return 0;   /* the idle task must never block */

    tasks[current].io_block_count++;
    return sched_block_current(deadline_tick);
}

volatile int sched_resched_pending = 0;

void sched_request_resched(void)
{
    sched_resched_pending = 1;
}

void sched_wake_io(struct task *t)
{
    if (!t || t->state != TASK_BLOCKED)
        return;
    uint64_t now = (uint64_t)timer_ticks();
    if (t->wait_start_tick && now >= t->wait_start_tick)
        t->wait_ticks += now - t->wait_start_tick;
    t->io_deadline_tick = 0;
    t->state = TASK_READY;
}

/*
 * Record one context switch into the trace ring.
 *
 * Called from sched_tick() immediately after the policy picks 'next', so the
 * snapshot reflects exactly what the policy observed when deciding. Runs in
 * IRQ or syscall context: no allocation, no blocking, plain stores only.
 */
static void trace_record_switch(uint64_t tick, uint32_t old, uint32_t next)
{
    uint32_t slot;
    if (trace_count < TRACE_RING_SIZE) {
        slot = (trace_head + trace_count) % TRACE_RING_SIZE;
        trace_count++;
    } else {
        /* Ring full: overwrite the oldest record. */
        slot = trace_head;
        trace_head = (trace_head + 1) % TRACE_RING_SIZE;
    }

    struct switch_record *r = &trace_ring[slot];
    r->tick        = tick;
    r->prev_idx    = old;
    r->next_idx    = next;
    r->task_count  = task_count < TRACE_MAX_TASKS ? task_count : TRACE_MAX_TASKS;
    r->_pad        = 0;
    for (uint32_t i = 0; i < r->task_count; i++) {
        r->tasks[i].pid           = tasks[i].pid;
        r->tasks[i].state         = (uint8_t)tasks[i].state;
        r->tasks[i]._pad[0]       = 0;
        r->tasks[i]._pad[1]       = 0;
        r->tasks[i]._pad[2]       = 0;
        r->tasks[i].total_ticks   = tasks[i].total_ticks;
        r->tasks[i].wait_ticks    = tasks[i].wait_ticks;
        r->tasks[i].syscall_count = tasks[i].syscall_count;
        r->tasks[i].io_block_count = tasks[i].io_block_count;
    }
}

void sched_tick(uint64_t tick)
{
    if (task_count <= 1)
        return;

    sched_resched_pending = 0;   /* any scheduler run satisfies requests */

    /* Deadline wakeups for I/O-blocked tasks: if an IRQ never arrived,
     * the waiter still wakes by its deadline and its caller falls back
     * to polling, so a lost interrupt degrades reads instead of
     * hanging them. */
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_BLOCKED && tasks[i].io_deadline_tick
            && tick >= tasks[i].io_deadline_tick) {
            if (tasks[i].wait_start_tick && tick >= tasks[i].wait_start_tick)
                tasks[i].wait_ticks += tick - tasks[i].wait_start_tick;
            tasks[i].io_deadline_tick = 0;
            tasks[i].state = TASK_READY;
        }
    }

    uint32_t old  = current;
    uint32_t next = active_policy->pick_next(current, tasks, task_count, tick);

    /* Accumulate runtime for the outgoing task on every tick, even when
     * the policy keeps the current task (next == old). Otherwise a
     * never-preempted task would show zero total_ticks. */
    if (tasks[old].last_scheduled_tick <= tick) {
        tasks[old].total_ticks += tick - tasks[old].last_scheduled_tick;
        tasks[old].last_scheduled_tick = tick;
    }

    if (next == old)
        return; /* nothing else to run */

    /* Capture the decision and the run-queue state the policy observed. */
    trace_record_switch(tick, old, next);

    /* Only mark old task READY if it hasn't already exited.
     * sched_current_exit() marks the task DEAD before calling sched_tick();
     * without this guard sched_tick would immediately revive it as a zombie,
     * causing repeated preemptions inside sched_current_exit's hlt loop and
     * an eventual kernel-stack overflow. */
    if (tasks[old].state != TASK_DEAD && tasks[old].state != TASK_WAITING
        && tasks[old].state != TASK_BLOCKED)
        tasks[old].state = TASK_READY;

    /* Start wait-time accounting if the task is now blocked. */
    if (tasks[old].state == TASK_WAITING || tasks[old].state == TASK_BLOCKED)
        tasks[old].wait_start_tick = tick;

    tasks[next].state              = TASK_RUNNING;
    tasks[next].last_scheduled_tick = tick;
    current = next;

    /* Update TSS RSP0 to the top of the new task's kernel stack
     * so that interrupts/syscalls from ring 3 land in the right place. */
    if (tasks[next].stack)
        tss_set_rsp0((uint64_t)(tasks[next].stack + TASK_STACK_SIZE));

    /* Switch address space if necessary (no-op for tasks sharing the same PML4). */
    if (tasks[next].cr3 != tasks[old].cr3)
        vmm_switch(tasks[next].cr3);

    context_switch(&tasks[old].rsp, tasks[next].rsp);
}

/* -----------------------------------------------------------------------
 * Trampoline — entered when a new task runs for the first time.
 * Reads the entry function from the task struct - no asm tricks needed.
 * ----------------------------------------------------------------------- */
static void task_entry_trampoline(void)
{
    /* Re-enable interrupts. We arrived here via context_switch inside an
     * IRQ handler, bypassing the iretq that would normally restore IF=1.
     * Without this sti the scheduler never fires again and only this task runs. */
    __asm__ volatile("sti");

    void (*func)(void) = tasks[current].entry;
    func();

    /* If the task function returns, mark it dead. */
    tasks[current].state = TASK_DEAD;
    for (;;) __asm__ volatile("hlt");
}

/* -----------------------------------------------------------------------
 * Ring-3 task creation
 * ----------------------------------------------------------------------- */

/*
 * Trampoline for ring-3 tasks.
 * Called via the normal context_switch ret path.
 * The iretq frame is already on the kernel stack (set up by user_task_create).
 * We just need to reload the user segment registers and execute iretq.
 */
static void user_entry_trampoline(void)
{
    /* Reload user data segments so DS/ES/FS/GS point to ring-3 data. */
    __asm__ volatile(
        "mov %0, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        :: "i"(0x23)   /* GDT_USER_DATA | RPL3 */
        : "ax"
    );
    /* iretq pops: RIP, CS, RFLAGS, RSP, SS — all set up by user_task_create */
    __asm__ volatile("iretq");
    __builtin_unreachable();
}

uint32_t user_task_create(const char *name, uint64_t cr3,
                          uint64_t entry, uint64_t ustack)
{
    if (task_count >= MAX_TASKS) return 0;

    struct task *t = &tasks[task_count];

    t->stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!t->stack) return 0;

    for (uint32_t i = 0; i < TASK_STACK_SIZE; i++)
        t->stack[i] = 0;

    fd_init_std(t);
    t->state = TASK_READY;
    t->pid   = next_pid++;
    t->name  = name;
    t->entry = (void (*)(void))0;   /* not used for ring-3 tasks */
    t->cr3   = cr3;
    /* Process group (Phase 18 inc.4): inherit the
     * spawner's group; a spawner with no group (a
     * kernel task -- the boot spawn of the shell)
     * makes the child a group leader (pgid = its pid).
     * Set here, inside creation, so the value is right
     * before the child can first be scheduled. */
    {
        struct task *spawner = sched_current_task();
        t->pgid = (spawner && spawner->pgid) ? spawner->pgid : t->pid;
    }
    t->parent_pid = 0;
    t->exit_code  = 0;
    t->wait_pid   = 0;

    /*
     * Build the kernel stack so that context_switch's ret lands in
     * user_entry_trampoline, which then executes iretq with this frame:
     *
     *  High address (stack top)
     *  ┌─────────────────┐
     *  │ SS  (0x23)      │  ← user data selector
     *  │ RSP (ustack)    │  ← user stack pointer
     *  │ RFLAGS          │  ← IF=1, IOPL=0
     *  │ CS  (0x1B)      │  ← user code selector
     *  │ RIP (entry)     │  ← user ELF entry point
     *  ├─────────────────┤  ← iretq reads from here upward
     *  │ r15=0           │
     *  │ r14=0           │
     *  │ r13=0           │
     *  │ r12=0           │
     *  │ rbx=0           │
     *  │ rbp=0           │
     *  │ RIP=trampoline  │  ← context_switch ret pops this
     *  └─────────────────┘  ← t->rsp points here
     *  Low address
     */
    uint64_t *sp = (uint64_t *)(t->stack + TASK_STACK_SIZE);

    /* iretq frame (pushed in reverse — high address first) */
    *--sp = 0x23;               /* SS  */
    *--sp = ustack;             /* RSP */
    *--sp = 0x202;              /* RFLAGS: IF=1, reserved bit 1 set */
    *--sp = 0x1B;               /* CS  */
    *--sp = entry;              /* RIP */

    /* context_switch frame */
    *--sp = (uint64_t)user_entry_trampoline;  /* ret address */
    *--sp = 0;  /* r15 */
    *--sp = 0;  /* r14 */
    *--sp = 0;  /* r13 */
    *--sp = 0;  /* r12 */
    *--sp = 0;  /* rbx */
    *--sp = 0;  /* rbp  ← rsp points here */

    t->rsp = (uint64_t)sp;

    /* Heap starts just above 2TB mark — safely in PML4[4], away from
     * the code (PML4[2] at 1TB) and the boot identity map (PML4[0]). */
    t->heap_base = 0x20000000000UL;  /* 2TB */
    t->heap_end  = 0x20000000000UL;  /* nothing allocated yet */

    task_count++;
    return t->pid;
}
