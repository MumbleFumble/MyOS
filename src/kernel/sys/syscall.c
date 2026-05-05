#include "syscall.h"
#include "../arch/idt.h"
#include "../arch/gdt.h"
#include "../arch/port_io.h"
#include "../proc/sched.h"
#include "../mem/vmm.h"
#include "../mem/pmm.h"
#include "../drivers/vga.h"
#include "../drivers/keyboard.h"

/* -----------------------------------------------------------------------
 * Serial helpers — kept for kernel-side debug; sys_write goes to VGA.
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

/* SYS_WRITE: write(fd, buf, count) */
static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (fd != FD_STDOUT && fd != FD_STDERR)
        return -1;
    if (count == 0)
        return 0;

    const char *s = (const char *)buf;
    /* Write to VGA screen and mirror to serial */
    vga_write(s, count);
    serial_write(s, count);
    return (int64_t)count;
}

/* SYS_READ: read(fd, buf, count)
 * Reads up to `count` bytes from the keyboard into buf.
 * Blocks until at least one character is available.
 * Stops early on newline (which is included in the returned data). */
static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (fd != FD_STDIN)
        return -1;
    if (count == 0)
        return 0;

    char *dst = (char *)buf;
    uint64_t n = 0;

    /* Block for the first character */
    dst[n++] = keyboard_getchar_wait();
    /* If that was a newline, return immediately */
    if (dst[n - 1] == '\n' || dst[n - 1] == '\r') {
        dst[n - 1] = '\n';
        return (int64_t)n;
    }

    /* Non-blocking drain for the rest */
    while (n < count) {
        char c = keyboard_getchar();
        if (!c) break;
        dst[n++] = c;
        if (c == '\n' || c == '\r') {
            dst[n - 1] = '\n';
            break;
        }
    }
    return (int64_t)n;
}

/* SYS_EXIT: exit(status) */
static int64_t sys_exit(uint64_t status)
{
    (void)status;
    sched_current_exit();
    for (;;) __asm__ volatile("hlt");
    __builtin_unreachable();
}

/* SYS_SBRK: sbrk(increment)
 * Grows (or queries, if increment==0) the calling task's heap.
 * Returns the old break on success, -1 on failure (OOM).
 * Increment is treated as a signed value to allow shrinking too,
 * but we only support growth for now. */
static int64_t sys_sbrk(uint64_t increment)
{
    struct task *t = sched_current_task();
    if (!t->cr3) return -1;   /* kernel tasks have no user heap */

    uint64_t old_break = t->heap_end;

    if (increment == 0)
        return (int64_t)old_break;

    uint64_t new_break = old_break + increment;

    /* Map pages for the newly requested range */
    uint64_t page_start = (old_break + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t page_end   = (new_break + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t va = page_start; va < page_end; va += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_page();
        if (!frame) return -1;
        /* Zero the frame (identity-mapped, physical == virtual in kernel) */
        uint64_t *p = (uint64_t *)frame;
        for (int i = 0; i < 512; i++) p[i] = 0;
        vmm_map_page_in(t->cr3, frame, va,
                        PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }

    t->heap_end = new_break;
    return (int64_t)old_break;
}

/* SYS_CLEAR: clear() — wipe the VGA screen and reset the cursor */
static int64_t sys_clear(void)
{
    vga_clear();
    return 0;
}

/* -----------------------------------------------------------------------
 * Dispatch table
 * ----------------------------------------------------------------------- */

int64_t syscall_dispatch(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    switch (nr) {
    case SYS_WRITE: return sys_write(arg1, arg2, arg3);
    case SYS_READ:  return sys_read(arg1, arg2, arg3);
    case SYS_EXIT:  return sys_exit(arg1);
    case SYS_SBRK:  return sys_sbrk(arg1);
    case SYS_CLEAR: return sys_clear();
    default:        return -1;  /* ENOSYS */
    }
}

/* -----------------------------------------------------------------------
 * Init — install int 0x80 trap gate
 * ----------------------------------------------------------------------- */

extern void isr80(void);

void syscall_init(void)
{
    idt_set_gate(0x80, (uint64_t)isr80, GDT_KERNEL_CODE, 0xEF);
}
