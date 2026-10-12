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
    while ((t[next].state == TASK_DEAD || t[next].state == TASK_WAITING)
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
    tasks[0].cr3   = vmm_current_cr3();
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

    t->state = TASK_READY;
    t->pid   = next_pid++;
    t->name  = name;
    t->entry = func;
    t->cr3   = vmm_current_cr3();   /* kernel tasks share the boot address space */
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

void sched_current_exit(void)
{
    tasks[current].state     = TASK_DEAD;
    tasks[current].exit_code = 0;  /* default; sys_exit sets it before calling us */

    /* Wake any task waiting for this pid */
    uint32_t dead_pid = tasks[current].pid;
    uint64_t now = (uint64_t)timer_ticks();
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_WAITING &&
            (tasks[i].wait_pid == 0 || tasks[i].wait_pid == dead_pid))
        {
            /* Accumulate the time this task spent blocked. wait_start_tick
             * was recorded when the task entered TASK_WAITING. */
            tasks[i].wait_ticks += now - tasks[i].wait_start_tick;
            tasks[i].state = TASK_READY;
        }
    }

    __asm__ volatile("sti");
    sched_tick((uint64_t)timer_ticks());
    for (;;) __asm__ volatile("hlt");
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
    /* No dead child yet — block until one finishes */
    /* First verify there is a matching live child */
    int has_child = 0;
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].parent_pid != tasks[current].pid) continue;
        if (pid != 0 && tasks[i].pid != pid) continue;
        if (tasks[i].state != TASK_DEAD) { has_child = 1; break; }
    }
    if (!has_child) return -1;

    tasks[current].state    = TASK_WAITING;
    tasks[current].wait_pid = pid;
    __asm__ volatile("sti");
    sched_tick((uint64_t)timer_ticks());
    /* After being woken we are RUNNING again; re-check */
    goto check;
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
    if (tasks[old].state != TASK_DEAD && tasks[old].state != TASK_WAITING)
        tasks[old].state = TASK_READY;

    /* Start wait-time accounting if the task is now blocked. */
    if (tasks[old].state == TASK_WAITING)
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

    t->state = TASK_READY;
    t->pid   = next_pid++;
    t->name  = name;
    t->entry = (void (*)(void))0;   /* not used for ring-3 tasks */
    t->cr3   = cr3;
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
