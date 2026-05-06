#include "syscall.h"
#include "../arch/idt.h"
#include "../arch/gdt.h"
#include "../arch/port_io.h"
#include "../proc/sched.h"
#include "../proc/elf.h"
#include "../fs/vfs.h"
#include "../mem/vmm.h"
#include "../mem/pmm.h"
#include "../mem/kheap.h"
#include "../drivers/vga.h"
#include "../drivers/keyboard.h"
#include "../drivers/rtc.h"

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

/* SYS_READ: read(fd, buf, count) */
static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (count == 0) return 0;

    /* stdin: keyboard */
    if (fd == FD_STDIN) {
        char *dst = (char *)buf;
        uint64_t n = 0;
        dst[n++] = keyboard_getchar_wait();
        if (dst[n - 1] == '\n' || dst[n - 1] == '\r') {
            dst[n - 1] = '\n';
            return (int64_t)n;
        }
        while (n < count) {
            char c = keyboard_getchar();
            if (!c) break;
            dst[n++] = c;
            if (c == '\n' || c == '\r') { dst[n - 1] = '\n'; break; }
        }
        return (int64_t)n;
    }

    /* file fd */
    if (fd >= 3 && fd < MAX_FDS) {
        struct task *t = sched_current_task();
        struct vfs_node *node = (struct vfs_node *)t->fds[fd].node;
        if (!node) return -1;
        int64_t r = vfs_read(node, t->fds[fd].offset, count, (uint8_t *)buf);
        if (r > 0) t->fds[fd].offset += (uint64_t)r;
        return r;
    }

    return -1;
}

/* SYS_EXIT: exit(status) */
static int64_t sys_exit(uint64_t status)
{
    sched_current_task()->exit_code = (int32_t)status;
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

/* SYS_GETPID: getpid() — return the calling task's pid */
static int64_t sys_getpid(void)
{
    return (int64_t)sched_current_task()->pid;
}

/* SYS_WAIT: wait(pid) — block until child exits, return its exit code */
static int64_t sys_wait(uint64_t pid)
{
    return (int64_t)sched_wait_pid((uint32_t)pid);
}

/* SYS_EXEC: exec(name, cmdline) — load a named program from the VFS and run it.
 * arg1 = name virtual address (just the program name, looked up in VFS)
 * arg2 = cmdline virtual address (full command line including progname, or 0)
 * Returns the new task's pid, or -1 on error. */
static int64_t sys_exec(uint64_t name_va, uint64_t cmdline_va)
{
    const char *name    = (const char *)name_va;
    const char *cmdline = cmdline_va ? (const char *)cmdline_va : name;

    /* First try the VFS (ramfs + future disk) */
    vfs_node_t *node = vfs_open(name);
    if (!node) return -1;

    /* Read the entire file into a kernel buffer via kheap */
    uint64_t fsz = node->size;
    uint8_t *buf = (uint8_t *)kmalloc(fsz);
    if (!buf) { vfs_close(node); return -1; }
    vfs_read(node, 0, fsz, buf);
    vfs_close(node);

    elf_result_t er;
    int r = elf_load(buf, fsz, cmdline, &er);
    kfree(buf);
    if (r != 0) return -1;

    uint32_t caller_pid = sched_current_task()->pid;
    uint32_t child_pid  = user_task_create(name, er.cr3, er.entry, er.ustack);
    if (child_pid == 0) return -1;

    struct task *child = sched_find_by_pid(child_pid);
    if (child) child->parent_pid = caller_pid;

    return (int64_t)child_pid;
}

/* SYS_OPEN: open(path) -> fd, or -1 on not-found / table full */
static int64_t sys_open(uint64_t path_va)
{
    const char *path = (const char *)path_va;
    vfs_node_t *node = vfs_open(path);
    if (!node) return -1;

    struct task *t = sched_current_task();
    for (int fd = 3; fd < MAX_FDS; fd++) {
        if (t->fds[fd].node == 0) {
            t->fds[fd].node   = node;
            t->fds[fd].offset = 0;
            return (int64_t)fd;
        }
    }
    vfs_close(node);   /* no free slot */
    return -1;
}

/* SYS_CLOSE: close(fd) -> 0 or -1 */
static int64_t sys_close(uint64_t fd_u)
{
    int fd = (int)fd_u;
    if (fd < 3 || fd >= MAX_FDS) return -1;
    struct task *t = sched_current_task();
    if (!t->fds[fd].node) return -1;
    vfs_close((vfs_node_t *)t->fds[fd].node);
    t->fds[fd].node   = 0;
    t->fds[fd].offset = 0;
    return 0;
}

/* SYS_READDIR: readdir(index, name_buf) -> 0 or -1 */
static int64_t sys_readdir(uint64_t index, uint64_t name_buf_va)
{
    return (int64_t)vfs_readdir((uint32_t)index, (char *)name_buf_va);
}

/* SYS_TIME: time(buf) — write current RTC time into caller's struct */
static int64_t sys_time(uint64_t buf_va)
{
    rtc_time_t *t = (rtc_time_t *)buf_va;
    rtc_read(t);
    return 0;
}

/* -----------------------------------------------------------------------
 * Dispatch table
 * ----------------------------------------------------------------------- */

int64_t syscall_dispatch(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    switch (nr) {
    case SYS_WRITE:   return sys_write(arg1, arg2, arg3);
    case SYS_READ:    return sys_read(arg1, arg2, arg3);
    case SYS_EXIT:    return sys_exit(arg1);
    case SYS_SBRK:    return sys_sbrk(arg1);
    case SYS_CLEAR:   return sys_clear();
    case SYS_GETPID:  return sys_getpid();
    case SYS_WAIT:    return sys_wait(arg1);
    case SYS_EXEC:    return sys_exec(arg1, arg2);
    case SYS_OPEN:    return sys_open(arg1);
    case SYS_CLOSE:   return sys_close(arg1);
    case SYS_READDIR: return sys_readdir(arg1, arg2);
    case SYS_TIME:    return sys_time(arg1);
    default:          return -1;  /* ENOSYS */
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
