#pragma once

#include <stdint.h>

/* Maximum number of tasks the scheduler can hold. */
#define MAX_TASKS    32

/* Kernel stack size per task (8 KiB). */
#define TASK_STACK_SIZE  8192

typedef enum {
    TASK_READY   = 0,
    TASK_RUNNING = 1,
    TASK_DEAD    = 2,
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
};

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
 */
void sched_tick(void);

/* Called by sys_exit: mark current task dead and switch away immediately. */
void sched_current_exit(void);

/*
 * Create a ring-3 task from a pre-loaded ELF result.
 * Sets up a kernel stack with an iretq frame targeting the ELF entry point
 * in the given address space.  The task will drop to ring 3 when first scheduled.
 */
uint32_t user_task_create(const char *name, uint64_t cr3, uint64_t entry, uint64_t ustack);

/* Returns a pointer to the currently running task (never NULL after sched_init). */
struct task *sched_current_task(void);

/* Low-level context switch (context_switch.S). */
void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
