#include "sched.h"
#include "../mem/kheap.h"
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

void sched_tick(void)
{
    if (task_count <= 1)
        return;

    uint32_t old = current;

    /* Round-robin: find the next READY task. */
    uint32_t next = (current + 1) % task_count;
    uint32_t tries = 0;
    while (tasks[next].state == TASK_DEAD && tries < task_count) {
        next = (next + 1) % task_count;
        tries++;
    }

    if (next == old)
        return; /* nothing else to run */

    tasks[old].state   = TASK_READY;
    tasks[next].state  = TASK_RUNNING;
    current = next;

    /* Update TSS RSP0 to the top of the new task's kernel stack
     * so that interrupts/syscalls from ring 3 land in the right place. */
    if (tasks[next].stack)
        tss_set_rsp0((uint64_t)(tasks[next].stack + TASK_STACK_SIZE));

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
