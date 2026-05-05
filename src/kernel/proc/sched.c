#include "sched.h"
#include "../mem/kheap.h"
#include "../mem/vmm.h"
#include "../arch/gdt.h"

/* -----------------------------------------------------------------------
 * Task table
 * ----------------------------------------------------------------------- */

static struct task tasks[MAX_TASKS];
static uint32_t   task_count = 0;
static uint32_t   current    = 0;   /* index of currently running task */
static uint32_t   next_pid   = 1;

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
    return t->pid;
}

void sched_current_exit(void)
{
    tasks[current].state     = TASK_DEAD;
    tasks[current].exit_code = 0;  /* default; sys_exit sets it before calling us */

    /* Wake any task waiting for this pid */
    uint32_t dead_pid = tasks[current].pid;
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_WAITING &&
            (tasks[i].wait_pid == 0 || tasks[i].wait_pid == dead_pid))
        {
            tasks[i].state = TASK_READY;
        }
    }

    __asm__ volatile("sti");
    sched_tick();
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
    sched_tick();
    /* After being woken we are RUNNING again; re-check */
    goto check;
}

void sched_tick(void)
{
    if (task_count <= 1)
        return;

    uint32_t old = current;

    /* Round-robin: find the next READY task. */
    uint32_t next = (current + 1) % task_count;
    uint32_t tries = 0;
    while ((tasks[next].state == TASK_DEAD || tasks[next].state == TASK_WAITING)
           && tries < task_count) {
        next = (next + 1) % task_count;
        tries++;
    }

    if (next == old)
        return; /* nothing else to run */

    /* Only mark old task READY if it hasn't already exited.
     * sched_current_exit() marks the task DEAD before calling sched_tick();
     * without this guard sched_tick would immediately revive it as a zombie,
     * causing repeated preemptions inside sched_current_exit's hlt loop and
     * an eventual kernel-stack overflow. */
    if (tasks[old].state != TASK_DEAD && tasks[old].state != TASK_WAITING)
        tasks[old].state = TASK_READY;
    tasks[next].state  = TASK_RUNNING;
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
