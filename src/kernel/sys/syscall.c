#include "syscall.h"
#include "../arch/idt.h"
#include "../arch/gdt.h"
#include "../arch/port_io.h"
#include "../proc/sched.h"

/* -----------------------------------------------------------------------
 * Serial helpers (COM1) — used by sys_write
 * ----------------------------------------------------------------------- */

static void serial_putchar(char c)
{
    while (!(inb(0x3F8 + 5) & 0x20));
    outb(0x3F8, (uint8_t)c);
}

static void serial_write(const char *buf, uint64_t len)
{
    for (uint64_t i = 0; i < len; i++)
        serial_putchar(buf[i]);
}

/* -----------------------------------------------------------------------
 * Syscall handlers
 * ----------------------------------------------------------------------- */

/* SYS_WRITE: write(fd, buf, count)
 * fd 1/2 both go to serial for now (no VGA cursor yet).
 * Returns bytes written, or -1 on bad args. */
static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (fd != FD_STDOUT && fd != FD_STDERR)
        return -1;
    if (count == 0)
        return 0;

    /* buf is a kernel virtual address — safe to dereference (identity mapped). */
    serial_write((const char *)buf, count);
    return (int64_t)count;
}

/* SYS_EXIT: exit(status)
 * Marks the current task dead and yields to the scheduler.
 * Does not return. */
static int64_t sys_exit(uint64_t status)
{
    (void)status;
    sched_current_exit();
    /* sched_current_exit() enables interrupts and context-switches away.
     * We should never reach here. */
    for (;;) __asm__ volatile("hlt");
    __builtin_unreachable();
}

/* -----------------------------------------------------------------------
 * Dispatch table
 * ----------------------------------------------------------------------- */

int64_t syscall_dispatch(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    switch (nr) {
    case SYS_WRITE: return sys_write(arg1, arg2, arg3);
    case SYS_EXIT:  return sys_exit(arg1);
    default:        return -1;  /* ENOSYS */
    }
}

/* -----------------------------------------------------------------------
 * Init — install int 0x80 trap gate
 * ----------------------------------------------------------------------- */

extern void isr80(void);

void syscall_init(void)
{
    /*
     * 0xEF = Present, DPL=3 (user-callable), 64-bit trap gate.
     * Trap gate (vs interrupt gate) preserves IF — interrupts stay enabled
     * during syscall handling, allowing the scheduler to preempt long syscalls.
     */
    idt_set_gate(0x80, (uint64_t)isr80, GDT_KERNEL_CODE, 0xEF);
}
