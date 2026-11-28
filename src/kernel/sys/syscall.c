#include "syscall.h"
#include "uaccess.h"
#include "../arch/idt.h"
#include "../arch/gdt.h"
#include "../arch/port_io.h"
#include "../proc/sched.h"
#include "../proc/policy_wf.h"
#include "../proc/policy_ml.h"
#include "../arch/exceptions.h"
#include "../proc/elf.h"
#include "../fs/vfs.h"
#include "../fs/diskfs.h"
#include "../mem/vmm.h"
#include "../mem/pmm.h"
#include "../mem/kheap.h"
#include "../drivers/vga.h"
#include "../drivers/keyboard.h"
#include "../drivers/ata.h"
#include "../drivers/e1000.h"
#include "../drivers/net.h"
#include "../arch/timer.h"
#include "../fs/bcache.h"
#include "../fs/prefetch.h"
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

/* -----------------------------------------------------------------------
 * fd ops (Phase 14 inc.1): every fd dispatches read/write/close
 * through its per-fd ops table; the handlers below contain no
 * fd-number special cases. A NULL op means "not supported on
 * this fd" and the dispatch returns -1, exactly as the old
 * number-based dispatch did (write to stdin, read from stdout,
 * closing the console). The op bodies are the old handler
 * bodies, moved verbatim.
 * ----------------------------------------------------------------------- */

/* Console stdin: the old sys_read FD_STDIN block, verbatim. */
static int64_t console_read(struct file *f, uint8_t *buf, uint64_t count)
{
    (void)f;
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

/* Console stdout/stderr: the old sys_write console block. */
static int64_t console_write(struct file *f, const uint8_t *buf,
                             uint64_t count)
{
    (void)f;
    if (count == 0)
        return 0;
    const char *s = (const char *)buf;
    /* Write to VGA screen and mirror to serial */
    vga_write(s, count);
    serial_write(s, count);
    return (int64_t)count;
}

/* File fds: the old sys_read/sys_write file branches, with the
 * offset owned by the fd slot. */
static int64_t file_read(struct file *f, uint8_t *buf, uint64_t count)
{
    int64_t r = vfs_read((vfs_node_t *)f->node, f->offset, count, buf);
    if (r > 0) f->offset += (uint64_t)r;
    return r;
}

static int64_t file_write(struct file *f, const uint8_t *buf,
                          uint64_t count)
{
    int64_t w = vfs_write((vfs_node_t *)f->node, f->offset, count,
                          (uint8_t *)buf);
    if (w > 0) f->offset += (uint64_t)w;
    return w;
}

static int64_t file_close(struct file *f)
{
    vfs_close((vfs_node_t *)f->node);
    return 0;
}

static const struct fd_ops console_in_ops = {
    .read = console_read, .write = 0, .close = 0
};
static const struct fd_ops console_out_ops = {
    .read = 0, .write = console_write, .close = 0
};
static const struct fd_ops file_ops = {
    .read = file_read, .write = file_write, .close = file_close
};

/* -----------------------------------------------------------------------
 * Shared descriptions (Phase 14 inc.2). Slots hold pointers;
 * NULL is the free-slot test. File descriptions come from a
 * fixed pool sized MAX_TASKS * MAX_FDS, so exhaustion is
 * unreachable while tasks are never recycled (the -1 paths
 * stay for when that changes); refcount 0 marks a free pool
 * entry. The console descriptions are static shared objects:
 * their ops never touch node/offset, so every task may point
 * at the same ones. Refcount/pool mutations run with IRQs
 * off (syscalls here are preemptible: the 0x80 gate is a
 * trap gate); backend closes always run with IRQs on.
 * ----------------------------------------------------------------------- */
#define FD_DESC_POOL (MAX_TASKS * MAX_FDS)
static struct file desc_pool[FD_DESC_POOL];

static struct file console_in_desc = {
    .node = 0, .ops = &console_in_ops, .offset = 0,
    .refcount = 0, .flags = FILE_STATIC
};
static struct file console_out_desc = {
    .node = 0, .ops = &console_out_ops, .offset = 0,
    .refcount = 0, .flags = FILE_STATIC
};

static uint64_t fd_cli_save(void)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    __asm__ volatile("cli");
    return rflags;
}
static void fd_restore(uint64_t rflags)
{
    if (rflags & 0x200) __asm__ volatile("sti");
}

static void fd_fatal(const char *msg)
{
    serial_write("fd: FATAL: ", 11);
    uint64_t n = 0; while (msg[n]) n++;
    serial_write(msg, n);
    serial_write("\r\n", 2);
    for (;;) __asm__ volatile("hlt");
    __builtin_unreachable();
}

static void desc_get(struct file *f)
{
    if (f->flags & FILE_STATIC) return;
    uint64_t fl = fd_cli_save();
    if (f->refcount <= 0) { fd_restore(fl); fd_fatal("desc_get: dead desc"); }
    f->refcount++;
    fd_restore(fl);
}

static void desc_put(struct file *f)
{
    if (f->flags & FILE_STATIC) return;
    uint64_t fl = fd_cli_save();
    if (f->refcount <= 0) { fd_restore(fl); fd_fatal("desc_put: dead desc"); }
    if (f->refcount > 1) { f->refcount--; fd_restore(fl); return; }
    fd_restore(fl);
    /* Last reference: the entry stays reserved (refcount still
     * 1, and no slot points at it) while the backend close runs
     * with IRQs on; it returns to the pool only afterwards. */
    if (f->ops && f->ops->close) f->ops->close(f);
    fl = fd_cli_save();
    f->node = 0; f->ops = 0; f->offset = 0; f->flags = 0;
    f->refcount = 0;
    fd_restore(fl);
}

/* Drop the reference held by slot fd of task t (if any) and
 * clear the slot, including its pin. Backend close happens
 * inside desc_put when the last alias goes away. */
static void fd_release(struct task *t, int fd)
{
    struct file *f = t->fds[fd];
    t->fds[fd] = 0;
    t->fd_pinned &= (uint16_t)~(1u << fd);
    if (f) desc_put(f);
}

/* Phase 18 inc.2 (fd.h): release every fd slot of a
 * dying task, so description refcounts drop and backend
 * closes run at the last alias (a killed pipe writer
 * delivers EOF). Runs on every death path: sys_exit and
 * the fault kill in the dying task's own context,
 * SYS_KILL in the killer's context under cli. Pinned
 * console slots are released like any other: fd_release
 * clears the slot and desc_put returns immediately for
 * the FILE_STATIC console descriptions, so the shared
 * console is never counted, closed or freed -- only the
 * dying task's own slots go away. */
void fd_teardown_task(struct task *t)
{
    for (int fd = 0; fd < MAX_FDS; fd++)
        if (t->fds[fd]) fd_release(t, fd);
}

/* fd.h: all slots free except the pinned console fds 0/1/2. */
void fd_init_std(struct task *t)
{
    for (int i = 0; i < MAX_FDS; i++) t->fds[i] = 0;
    t->fds[0] = &console_in_desc;
    t->fds[1] = &console_out_desc;
    t->fds[2] = &console_out_desc;
    t->fd_pinned = 0x7;
}

/* The one place the free-slot test lives: slot == NULL. */
static struct file *fd_get(uint64_t fd)
{
    if (fd >= MAX_FDS) return 0;
    struct file *f = sched_current_task()->fds[fd];
    if (f && !f->ops) fd_fatal("fd_get: dead desc in slot");
    return f;
}

/* -----------------------------------------------------------------------
 * Pipes (Phase 14 inc.3). One struct pipe is a bounded ring
 * buffer shared by two descriptions: a read end and a write
 * end. readers/writers count live END DESCRIPTIONS (0 or 1
 * each), never aliases: the end close ops run once per end
 * (desc_put, at the end's last alias), so dup2 aliasing an
 * end is transparent to the pipe. The close that takes both
 * counts to 0 returns the pipe to its pool.
 *
 * Semantics are NON-BLOCKING (a task blocking on its own
 * pipe would deadlock; cross-task pipes do not exist yet):
 *   read:  buffered bytes first (short count allowed);
 *          empty + writers > 0 -> E_WOULDBLOCK (-2);
 *          empty + writers == 0 -> 0 (EOF, after draining).
 *   write: readers == 0 -> -1; no free space -> -2;
 *          otherwise copies what fits and returns the short
 *          count. Unread bytes are never dropped or
 *          overwritten. count == 0 returns 0 in both ops.
 * Ring mutations run in one IRQ-off section per call (the
 * state cannot be shared across tasks yet; the section is
 * bounded by the 1024-byte capacity).
 * ----------------------------------------------------------------------- */
#define PIPE_CAP 1024
#define PIPE_POOL_N 16

struct pipe {
    uint8_t  buf[PIPE_CAP];
    uint32_t head;      /* next byte to read */
    uint32_t tail;      /* next byte to write */
    uint32_t count;     /* bytes buffered (full vs empty) */
    int      readers;   /* live read-end descriptions: 0/1 */
    int      writers;   /* live write-end descriptions: 0/1 */
    int      in_use;
};
static struct pipe pipe_pool[PIPE_POOL_N];

static int64_t pipe_read(struct file *f, uint8_t *buf, uint64_t count)
{
    if (count == 0) return 0;
    struct pipe *pp = f->pipe;
    uint64_t fl = fd_cli_save();
    if (pp->count == 0) {
        int w = pp->writers;
        fd_restore(fl);
        return w ? E_WOULDBLOCK : 0;
    }
    uint64_t n = count < pp->count ? count : pp->count;
    for (uint64_t i = 0; i < n; i++) {
        buf[i] = pp->buf[pp->head];
        pp->head = (pp->head + 1) & (PIPE_CAP - 1);
    }
    pp->count -= (uint32_t)n;
    fd_restore(fl);
    return (int64_t)n;
}

static int64_t pipe_write(struct file *f, const uint8_t *buf,
                          uint64_t count)
{
    if (count == 0) return 0;
    struct pipe *pp = f->pipe;
    uint64_t fl = fd_cli_save();
    if (pp->readers == 0) { fd_restore(fl); return -1; }
    uint32_t freeb = PIPE_CAP - pp->count;
    if (freeb == 0) { fd_restore(fl); return E_WOULDBLOCK; }
    uint64_t n = count < freeb ? count : freeb;
    for (uint64_t i = 0; i < n; i++) {
        pp->buf[pp->tail] = buf[i];
        pp->tail = (pp->tail + 1) & (PIPE_CAP - 1);
    }
    pp->count += (uint32_t)n;
    fd_restore(fl);
    return (int64_t)n;
}

static int64_t pipe_read_close(struct file *f)
{
    struct pipe *pp = f->pipe;
    uint64_t fl = fd_cli_save();
    pp->readers--;
    if (pp->readers == 0 && pp->writers == 0) pp->in_use = 0;
    fd_restore(fl);
    return 0;
}

static int64_t pipe_write_close(struct file *f)
{
    struct pipe *pp = f->pipe;
    uint64_t fl = fd_cli_save();
    pp->writers--;
    if (pp->readers == 0 && pp->writers == 0) pp->in_use = 0;
    fd_restore(fl);
    return 0;
}

static const struct fd_ops pipe_read_ops = {
    .read = pipe_read, .write = 0, .close = pipe_read_close
};
static const struct fd_ops pipe_write_ops = {
    .read = 0, .write = pipe_write, .close = pipe_write_close
};

/* Reserve a pool description (refcount 1, zeroed, never
 * published). Paired with desc_return_raw for the rollback
 * path of a multi-description creation: nothing was ever
 * visible, so no close op runs. */
static struct file *desc_grab_raw(void)
{
    uint64_t fl = fd_cli_save();
    struct file *f = 0;
    for (int i = 0; i < FD_DESC_POOL; i++) {
        if (desc_pool[i].refcount == 0) { f = &desc_pool[i]; break; }
    }
    if (f) {
        f->node = 0; f->ops = 0; f->offset = 0; f->flags = 0;
        f->refcount = 1;
    }
    fd_restore(fl);
    return f;
}

static void desc_return_raw(struct file *f)
{
    uint64_t fl = fd_cli_save();
    f->node = 0; f->ops = 0; f->offset = 0; f->flags = 0;
    f->refcount = 0;
    fd_restore(fl);
}

/* SYS_PIPE: pipe(fds_out) -> 0 or -1. Installs the read end
 * in the lower of the two lowest free slots >= 3 and the
 * write end in the other. All-or-nothing: on any failure
 * nothing is installed and nothing leaks. */
static int64_t sys_pipe(uint64_t fds_va)
{
    if (!fds_va) return -1;   /* pinned behaviour (pipetest) */
    /* Phase 17 inc.3: the 16-byte out array (two int64_t,
     * matching the lib's long[2]) must be validated user
     * memory before anything is created or written. All
     * failure paths below run before the array write, so a
     * refusal leaves the caller's array untouched. */
    if (!user_range_ok(fds_va, 16, 1)) return EFAULT;
    struct task *t = sched_current_task();
    int rslot = -1, wslot = -1;
    for (int fd = 3; fd < MAX_FDS; fd++) {
        if (!t->fds[fd]) {
            if (rslot < 0) rslot = fd;
            else { wslot = fd; break; }
        }
    }
    if (rslot < 0 || wslot < 0) return -1;

    uint64_t fl = fd_cli_save();
    struct pipe *pp = 0;
    for (int i = 0; i < PIPE_POOL_N; i++) {
        if (!pipe_pool[i].in_use) { pp = &pipe_pool[i]; break; }
    }
    if (pp) {
        pp->head = pp->tail = pp->count = 0;
        pp->readers = 1; pp->writers = 1; pp->in_use = 1;
    }
    fd_restore(fl);
    if (!pp) return -1;

    struct file *fr = desc_grab_raw();
    struct file *fw = fr ? desc_grab_raw() : 0;
    if (!fr || !fw) {
        if (fr) desc_return_raw(fr);
        fl = fd_cli_save(); pp->in_use = 0; fd_restore(fl);
        return -1;
    }
    fr->pipe = pp; fr->ops = &pipe_read_ops;  fr->flags = FILE_PIPE_R;
    fw->pipe = pp; fw->ops = &pipe_write_ops; fw->flags = FILE_PIPE_W;

    /* Caller's array first, then publish the slots. */
    int64_t *out = (int64_t *)fds_va;
    out[0] = rslot;
    out[1] = wslot;
    t->fds[rslot] = fr;
    t->fds[wslot] = fw;
    return 0;
}

/* SYS_WRITE: write(fd, buf, count) */
static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count)
{
    /* Phase 17 inc.3: the whole buffer range must be
     * validated user memory before the fd op reads it. */
    if (!user_range_ok(buf, count, 0)) return EFAULT;
    struct file *f = fd_get(fd);
    if (!f || !f->ops->write) return -1;
    return f->ops->write(f, (const uint8_t *)buf, count);
}

/* SYS_READ: read(fd, buf, count) */
static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (count == 0) return 0;
    /* Phase 17 inc.3: the whole buffer range must be
     * validated writable user memory before the fd op
     * fills it. */
    if (!user_range_ok(buf, count, 1)) return EFAULT;
    struct file *f = fd_get(fd);
    if (!f || !f->ops->read) return -1;
    return f->ops->read(f, (uint8_t *)buf, count);
}

/* SYS_EXIT: exit(status) */
static int64_t sys_exit(uint64_t status)
{
    /* Phase 18 inc.2: the exit code, the fd teardown and
     * sched_current_exit's mark-and-wake form one cli
     * section (the cli here covers the teardown;
     * sched_current_exit clis itself), so a concurrent
     * SYS_KILL cannot interleave with a half-torn-down
     * exit or overwrite the chosen code: on a single
     * core the killer can only run while this task is
     * scheduled out, which cannot happen mid-section. */
    __asm__ volatile("cli");
    struct task *cur = sched_current_task();
    cur->exit_code = (int32_t)status;
    fd_teardown_task(cur);
    sched_current_exit();
    for (;;) __asm__ volatile("hlt");
    __builtin_unreachable();
}

/* SYS_KILL: kill(pid, sig) -> 0 / -1 (Phase 18 inc.2;
 * inc.3 added the signal). The target collects
 * EXIT_SIGNAL_BASE + sig through SYS_WAIT. The signal
 * is validated FIRST, before any state is touched --
 * including before the self-kill branch, so an invalid
 * signal can never terminate anything, not even the
 * caller. No permission model: any task may kill any
 * user task; kernel tasks are refused inside
 * sched_kill_task. Self-kill is exit with the encoded
 * status, through the same cli + teardown discipline
 * as sys_exit. */
static int64_t sys_kill(uint64_t pid, uint64_t sig)
{
    if (sig != SIGINT && sig != SIGKILL && sig != SIGTERM)
        return -1;
    /* Group form (Phase 18 inc.4, POSIX encoding): a
     * negative pid targets the process group -pid.
     * pid == 0 is NOT special-cased (POSIX would mean
     * the caller's own group): it stays an ordinary
     * nonexistent pid and returns -1, so a stray
     * kill(0, sig) can never wipe out the caller's
     * group. If the caller belongs to the target
     * group it is signalled like any member, and the
     * call finishes through the exit path instead of
     * returning into a dead task. */
    if ((int64_t)pid < 0) {
        int rc = sched_kill_group((uint32_t)(-(int64_t)pid),
                                  (int)sig);
        if (rc == 0 && sched_current_task()->state == TASK_DEAD) {
            sched_current_exit();
            for (;;) __asm__ volatile("hlt");
        }
        return rc;
    }
    struct task *cur = sched_current_task();
    if (cur && (uint32_t)pid == cur->pid) {
        __asm__ volatile("cli");
        cur->exit_code = EXIT_SIGNAL_BASE + (int32_t)sig;
        fd_teardown_task(cur);
        sched_current_exit();
        for (;;) __asm__ volatile("hlt");
        __builtin_unreachable();
    }
    return sched_kill_task((uint32_t)pid, (int)sig);
}

/* SYS_GETPGID / SYS_SETPGID (Phase 18 inc.4): thin
 * wrappers; the rules live with the task table in
 * sched.c (sched_getpgid / sched_setpgid). */
static int64_t sys_getpgid(uint64_t pid)
{
    return sched_getpgid((uint32_t)pid);
}

static int64_t sys_setpgid(uint64_t pid, uint64_t pgid)
{
    return sched_setpgid((uint32_t)pid, (uint32_t)pgid);
}

/* SYS_SETFG (Phase 18 inc.5): record the console's
 * foreground process group for Ctrl-C delivery,
 * together with the caller's pid (the recorded setter
 * is spared by the delivery -- see sched.h). Any user
 * task may set it; in practice only the shell does.
 * pgid 0 is refused: group 0 is the kernel's, and a
 * foreground record pointing at it could only ever
 * be a bug. */
static int64_t sys_setfg(uint64_t pgid)
{
    if (pgid == 0) return -1;
    struct task *cur = sched_current_task();
    sched_fg_set((uint32_t)pgid, cur ? cur->pid : 0);
    return 0;
}

/* SYS_TRYWAIT (Phase 18 inc.6): the WNOHANG analogue
 * of SYS_WAIT, for the shell's prompt-time job
 * reaping. Returns the child's exit code if it is
 * DEAD (repeatable, like SYS_WAIT -- task slots are
 * never reaped), -2 (E_WOULDBLOCK) if it is still
 * running, and -1 if the pid is not the caller's
 * child. A single state read; no lock needed. */
static int64_t sys_trywait(uint64_t pid)
{
    struct task *cur = sched_current_task();
    struct task *t = sched_find_by_pid((uint32_t)pid);
    if (!cur || !t || t->parent_pid != cur->pid) return -1;
    if (t->state != TASK_DEAD) return -2;
    return t->exit_code;
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
static int64_t sys_exec_impl(uint64_t name_va, uint64_t cmdline_va,
                             const int64_t *pairs, int npairs)
{
    /* Phase 17 inc.3: name and cmdline are copied through
     * the validated layer into kernel buffers first (name
     * bounded by the filesystem name limit, cmdline by the
     * shell's line length); the rest of this handler works
     * on the kernel copies only. */
    char kname[64];
    if (copy_string_from_user(kname, name_va, sizeof kname) < 0)
        return EFAULT;
    char kcmd[256];
    const char *cmdline = kname;
    if (cmdline_va) {
        if (copy_string_from_user(kcmd, cmdline_va, sizeof kcmd) < 0)
            return EFAULT;
        cmdline = kcmd;
    }
    const char *name = kname;

    /* Phase 17 inc.5: validate EVERY inheritance pair before
     * the child exists and before any refcount moves: both
     * fds in range (the unsigned compare also refuses the
     * negative int64 values a crafted map can carry), the
     * parent slot open, no duplicate child target. Any
     * refusal leaves no child and no moved refcount. The
     * parent's table cannot change between here and the
     * install: only a task's own syscalls write its table,
     * the parent is inside this one, and the exec body's
     * file access goes through the VFS directly, never the
     * caller's fd table. */
    struct task *parent = sched_current_task();
    if (pairs) {
        for (int i = 0; i < npairs; i++) {
            uint64_t cfd = (uint64_t)pairs[2 * i];
            uint64_t pfd = (uint64_t)pairs[2 * i + 1];
            if (cfd >= MAX_FDS || pfd >= MAX_FDS) return -1;
            if (!parent->fds[pfd]) return -1;
            for (int j = 0; j < i; j++)
                if ((uint64_t)pairs[2 * j] == cfd) return -1;
        }
    }

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

    /* Create + parentage + inheritance install run with
     * interrupts off (saved-flags restore, the fd idiom).
     * user_task_create marks the child TASK_READY at once,
     * so without this window the child could be scheduled
     * and observe its default console table, or open files
     * into slots the install is about to overwrite.
     * Nothing inside the window blocks or enables
     * interrupts (user_task_create only kmallocs and writes
     * the task struct; desc_get on a validated live
     * description cannot fail and never allocates). A slot
     * the install displaces can only hold a FILE_STATIC
     * console description or NULL (fd_init_std installs
     * nothing else and the child has never run), so
     * displacement needs no desc_put. */
    uint64_t fl = fd_cli_save();
    uint32_t child_pid = user_task_create(name, er.cr3, er.entry, er.ustack);
    struct task *child = child_pid ? sched_find_by_pid(child_pid) : 0;
    if (child) {
        child->parent_pid = parent->pid;
        if (pairs) {
            for (int i = 0; i < npairs; i++) {
                int cfd = (int)pairs[2 * i];
                int pfd = (int)pairs[2 * i + 1];
                struct file *f = parent->fds[pfd];   /* re-read; validated */
                desc_get(f);
                child->fds[cfd] = f;
                child->fd_pinned &= (uint16_t)~(1u << cfd);
            }
        }
    }
    fd_restore(fl);
    if (!child) return -1;
    return (int64_t)child_pid;
}

/* SYS_EXEC: plain spawn, console-only child (unchanged). */
static int64_t sys_exec(uint64_t name_va, uint64_t cmdline_va)
{
    return sys_exec_impl(name_va, cmdline_va, 0, 0);
}

/* SYS_EXEC_INHERIT: spawn with a chosen subset of the
 * caller's fd slots installed in the child. arg3 points to
 * a user map: int64 count (0..MAX_FDS), then count pairs
 * {child_fd, parent_fd}. The map is copied in through the
 * Phase 17 inc.3 layer first (bad pointer -> EFAULT; a NULL
 * map is simply a bad pointer here - callers wanting no
 * inheritance use SYS_EXEC); count is taken only from the
 * kernel copy. Semantic refusals return -1 from
 * sys_exec_impl's validation, before any child exists. */
static int64_t sys_exec_inherit(uint64_t name_va, uint64_t cmdline_va,
                                uint64_t map_va)
{
    int64_t hdr;
    if (copy_from_user(&hdr, map_va, sizeof hdr) < 0)
        return EFAULT;
    if (hdr < 0 || hdr > MAX_FDS) return -1;
    int64_t pairs[MAX_FDS * 2];
    if (hdr > 0
            && copy_from_user(pairs,
                              map_va + sizeof hdr,
                              (uint64_t)hdr * 2 * sizeof(int64_t)) < 0)
        return EFAULT;
    return sys_exec_impl(name_va, cmdline_va, pairs, (int)hdr);
}

static int64_t fd_alloc(struct task *t, vfs_node_t *node)
{
    if (!node) return -1;
    /* Slot first (per-task state), then a pool description;
     * the pointer is published last, fully initialised. */
    int slot = -1;
    for (int fd = 3; fd < MAX_FDS; fd++) {
        if (!t->fds[fd]) { slot = fd; break; }
    }
    if (slot < 0) { vfs_close(node); return -1; }   /* table full */

    uint64_t fl = fd_cli_save();
    struct file *f = 0;
    for (int i = 0; i < FD_DESC_POOL; i++) {
        if (desc_pool[i].refcount == 0) { f = &desc_pool[i]; break; }
    }
    if (f) {
        f->node     = node;
        f->ops      = &file_ops;
        f->offset   = 0;
        f->flags    = 0;
        f->refcount = 1;
        t->fds[slot] = f;
    }
    fd_restore(fl);
    if (!f) { vfs_close(node); return -1; }         /* pool empty */
    return (int64_t)slot;
}

/* SYS_OPEN: open(path) -> fd, or -1 on not-found / table full */
static int64_t sys_open(uint64_t path_va)
{
    char kpath[64];
    if (copy_string_from_user(kpath, path_va, sizeof kpath) < 0)
        return EFAULT;
    const char *path = kpath;
    vfs_node_t *node = vfs_open(path);
    if (!node) return -1;
    return fd_alloc(sched_current_task(), node);
}

/* SYS_FCREATE: fcreate(path) -> fd (Phase 6 inc.3). Creates the
 * file if needed, truncates it if it exists. */
static int64_t sys_fcreate(uint64_t path_va)
{
    char kpath[64];
    if (copy_string_from_user(kpath, path_va, sizeof kpath) < 0)
        return EFAULT;
    const char *path = kpath;
    vfs_node_t *node = vfs_create(path);
    if (!node) return -1;
    return fd_alloc(sched_current_task(), node);
}

/* SYS_CLOSE: close(fd) -> 0 or -1. Refuses pinned slots
 * (the fresh console fds 0/1/2, as closing them has always
 * failed) and free slots; anything else is released through
 * fd_release, so the backend close runs only when the last
 * alias closes. A console ALIAS (an unpinned slot pointing
 * at a console description) closes fine: it just unlinks. */
static int64_t sys_close(uint64_t fd_u)
{
    int fd = (int)fd_u;
    if (fd < 0 || fd >= MAX_FDS) return -1;
    struct task *t = sched_current_task();
    if (!t->fds[fd]) return -1;
    if (t->fd_pinned & (1u << fd)) return -1;
    fd_release(t, fd);
    return 0;
}

/* SYS_DUP2: dup2(oldfd, newfd) -> newfd or -1 (Phase 14
 * inc.2). Both slots end up pointing at oldfd's description:
 * one offset, backend close at the last release. A displaced
 * newfd is released first (bypassing the pin, which is what
 * makes dup2 over fd 1 usable for redirection). */
static int64_t sys_dup2(uint64_t oldfd, uint64_t newfd)
{
    if (oldfd >= MAX_FDS || newfd >= MAX_FDS) return -1;
    struct task *t = sched_current_task();
    struct file *f = t->fds[oldfd];
    if (!f) return -1;
    if (oldfd == newfd) return (int64_t)newfd;
    desc_get(f);                 /* the new alias's ref first */
    fd_release(t, (int)newfd);   /* then displace the old one */
    t->fds[newfd] = f;
    return (int64_t)newfd;
}

/* SYS_READDIR: readdir(index, name_buf) -> 0 or -1 */
static int64_t sys_readdir(uint64_t index, uint64_t name_buf_va)
{
    /* Phase 17 inc.3: the name buffer (VFS_NAME_MAX bytes,
     * written by the callee) must be validated user memory. */
    if (!user_range_ok(name_buf_va, VFS_NAME_MAX, 1)) return EFAULT;
    return (int64_t)vfs_readdir((uint32_t)index, (char *)name_buf_va);
}

/* SYS_TIME: time(buf) — write current RTC time into caller's struct */
static int64_t sys_time(uint64_t buf_va)
{
    rtc_time_t *t = (rtc_time_t *)buf_va;
    rtc_read(t);
    return 0;
}

/* SYS_TELEMETRY: telemetry(buf_va, max_tasks)
 * Copies up to max_tasks task_stat_t records into the caller's buffer.
 * Returns the number of records written, or -1 on bad args. */
static int64_t sys_telemetry(uint64_t buf_va, uint64_t max_tasks)
{
    if (!buf_va || max_tasks == 0 || max_tasks > MAX_TASKS)
        return -1;
    task_stat_t *buf = (task_stat_t *)buf_va;
    return (int64_t)sched_get_stats(buf, (int)max_tasks);
}

/* SYS_SETPOLICY: setpolicy(index) — swap the active scheduling policy.
 * index 0 = round-robin, 1 = weighted-fair, 2 = learned.
 * Returns 0 on success, -1 if the index is unknown. */
static int64_t sys_setpolicy(uint64_t index)
{
    switch (index) {
    case SCHED_POLICY_RR:
        sched_set_policy(0);  /* NULL reverts to built-in round-robin */
        return 0;
    case SCHED_POLICY_WF:
        sched_set_policy(&wf_policy);
        return 0;
    case SCHED_POLICY_ML:
        sched_set_policy(&ml_policy);
        return 0;
    default:
        return -1;
    }
}

/* SYS_TELEMETRY_DRAIN: telemetry_drain(buf_va, max_records)
 * Copies up to max_records switch_record entries (oldest first) into the
 * caller's buffer and clears the trace ring.
 * Returns the number of records written, or -1 on bad args. */
static int64_t sys_telemetry_drain(uint64_t buf_va, uint64_t max_records)
{
    if (!buf_va || max_records == 0 || max_records > 1024)
        return -1;
    struct switch_record *buf = (struct switch_record *)buf_va;
    return (int64_t)sched_trace_drain(buf, (int)max_records);
}

/* SYS_PF_DRAIN: pf_drain(buf_va, max_records)
 * Copies up to max_records pf_record entries (oldest first) into the
 * caller's buffer and clears the fault trace ring.
 * Returns the number of records written, or -1 on bad args. */
static int64_t sys_pf_drain(uint64_t buf_va, uint64_t max_records)
{
    if (!buf_va || max_records == 0 || max_records > 1024)
        return -1;
    struct pf_record *buf = (struct pf_record *)buf_va;
    return (int64_t)pf_trace_drain(buf, (int)max_records);
}

/* SYS_PF_SETPOLICY: pf_setpolicy(index) -> 0 or -1.
 * Index 0 = FIFO, 1 = learned. */
static int64_t sys_pf_setpolicy(uint64_t index)
{
    if (index == 0) { pf_set_policy(&pf_policy_fifo); return 0; }
    if (index == 1) { pf_set_policy(&pf_policy_learned); return 0; }
    return -1;
}

/* SYS_IO_DRAIN: io_drain(buf_va, max_records)
 * Copies up to max_records io_record entries (oldest first) into the
 * caller's buffer and clears the I/O trace ring.
 * Returns the number of records written, or -1 on bad args. */
static int64_t sys_io_drain(uint64_t buf_va, uint64_t max_records)
{
    if (!buf_va || max_records == 0 || max_records > 1024)
        return -1;
    struct io_record *buf = (struct io_record *)buf_va;
    return (int64_t)io_trace_drain(buf, (int)max_records);
}

/* SYS_BCACHE_STAT: bcache_stat(hits_va, misses_va)
 * Writes cache hit/miss counters to userspace. Returns 0 or -1. */
static int64_t sys_bcache_stat(uint64_t hits_va, uint64_t misses_va,
                               uint64_t dirty_va)
{
    if (!hits_va || !misses_va)
        return -1;
    *(uint64_t *)hits_va = bcache_hits();
    *(uint64_t *)misses_va = bcache_misses();
    if (dirty_va)
        *(uint64_t *)dirty_va = bcache_dirty_count();
    return 0;
}

/* SYS_NETSTAT (Phase 7): fill the caller's netstat struct. */
struct netstat_out {
    uint8_t  mac[6];
    uint8_t  present;
    uint8_t  link;
    uint64_t rx_frames;
    uint64_t tx_frames;
    uint32_t arp_rx;
    uint32_t arp_tx;
    uint32_t icmp_rx;
    uint32_t icmp_tx;
    uint32_t udp_rx;
    uint32_t udp_tx;
    uint32_t irq_rx;
    uint8_t  ip[4];
    uint8_t  gw[4];
    uint32_t bad_cksum;
};

static int64_t sys_netstat(uint64_t out_va)
{
    if (!out_va) return -1;
    struct netstat_out *o = (struct netstat_out *)out_va;
    o->present = (uint8_t)e1000_present();
    o->link    = (uint8_t)e1000_link_up();
    o->mac[0] = o->mac[1] = o->mac[2] = 0;
    o->mac[3] = o->mac[4] = o->mac[5] = 0;
    if (o->present) e1000_mac_addr(o->mac);
    e1000_stats(&o->rx_frames, &o->tx_frames);
    net_stack_stats(&o->arp_rx, &o->arp_tx, &o->icmp_rx, &o->icmp_tx,
                    &o->udp_rx, &o->udp_tx, &o->bad_cksum);
    o->irq_rx = e1000_rx_irqs();
    net_our_ip(o->ip);
    net_gateway(o->gw);
    return 0;
}

/* SYS_NETSEND (Phase 7): send the fixed test frame. */
static int64_t sys_netsend(void)
{
    return e1000_send_hello();
}

/* Resolve an on-link (or gateway) IP to a MAC via ARP, blocking
 * in 100 ms slices for up to ~3 s. 1 on success. */
static int resolve_mac(const uint8_t *ip, uint8_t mac[6])
{
    if (net_arp_lookup(ip, mac))
        return 1;
    net_arp_request(ip);
    for (int i = 0; i < 30; i++) {
        sched_block_current((uint64_t)timer_ticks() + 10);
        if (net_arp_lookup(ip, mac))
            return 1;
    }
    return 0;
}

/* SYS_PING (Phase 7): resolve ARP if needed, send an ICMP echo
 * request and block (in 100 ms slices) for the reply. Returns
 * the round-trip time in timer ticks (10 ms each) or -1. */
static int64_t sys_ping(uint64_t ip_va, uint64_t seq)
{
    if (!ip_va) return -1;
    const uint8_t *ip = (const uint8_t *)ip_va;
    uint8_t hop[4];
    net_route_ip(ip, hop);
    uint8_t mac[6];
    if (!resolve_mac(hop, mac))
        return -1;
    if (net_ping_send(ip, mac, (uint16_t)seq) != 0) return -1;
    for (int i = 0; i < 30; i++) {
        uint32_t rtt = 0;
        if (net_ping_result(&rtt) == 1) return (int64_t)rtt;
        sched_block_current((uint64_t)timer_ticks() + 10);
    }
    return -1;
}

/* SYS_DHCP (Phase 8): run the DHCP exchange, blocking in 100 ms
 * slices while the RX task collects the OFFER/ACK. On success
 * fills the caller's dhcp_out and returns 0. */
struct dhcp_out {
    uint8_t ip[4];
    uint8_t gw[4];
    uint8_t mask[4];
    uint8_t dns[4];
    uint32_t lease_secs;
    uint32_t renewals;
};

static int64_t sys_dhcp(uint64_t out_va)
{
    if (!out_va) return -1;
    net_dhcp_start();
    int offered = 0;
    for (int i = 0; i < 40 && !offered; i++) {
        if (net_dhcp_have_offer()) { offered = 1; break; }
        sched_block_current((uint64_t)timer_ticks() + 10);
    }
    if (!offered) return -1;
    net_dhcp_request();
    for (int i = 0; i < 40; i++) {
        int st = net_dhcp_state();
        if (st == 3) {
            struct dhcp_out *o = (struct dhcp_out *)out_va;
            net_dhcp_info(o->ip, o->gw, o->mask, o->dns,
                          &o->lease_secs, &o->renewals);
            return 0;
        }
        if (st < 0) return -1;
        sched_block_current((uint64_t)timer_ticks() + 10);
    }
    return -1;
}

/* SYS_NSLOOKUP (Phase 9): resolve a DNS name to an IPv4 address.
 * Server: arg2 pointer to 4 bytes, or 0 for the DHCP-learned
 * server. Blocks in 100 ms slices for up to ~4 s. */
static int64_t sys_nslookup(uint64_t name_va, uint64_t server_va,
                            uint64_t out_va)
{
    if (!name_va || !out_va) return -1;
    char name[128];
    const char *src = (const char *)name_va;
    int i = 0;
    while (i < 127 && src[i]) { name[i] = src[i]; i++; }
    name[i] = 0;
    if (i == 0) return -1;
    uint8_t server[4];
    if (server_va) {
        const uint8_t *sv = (const uint8_t *)server_va;
        for (int k = 0; k < 4; k++) server[k] = sv[k];
    } else {
        net_dns_server(server);
    }
    int zero = 1;
    for (int k = 0; k < 4; k++) if (server[k]) zero = 0;
    if (zero) return -1;
    uint8_t hop[4];
    net_route_ip(server, hop);
    uint8_t mac[6];
    if (!resolve_mac(hop, mac)) return -1;
    if (net_dns_begin(name, server, mac) != 0) return -1;
    uint8_t *out = (uint8_t *)out_va;
    for (int j = 0; j < 40; j++) {
        int r = net_dns_poll(out);
        if (r == 1) return 0;
        if (r < 0) return -1;
        sched_block_current((uint64_t)timer_ticks() + 10);
    }
    return -1;
}

/* SYS_TCPECHO (Phase 9): one TCP exchange. Connects to ip:port,
 * sends args->data, collects the reply into args->out until the
 * peer closes or the buffer fills, then closes. Returns the
 * byte count or -1. All waits block in 100 ms slices and drive
 * the TCP retransmit timer. */
struct tcpecho_args {
    const char *data;
    unsigned int len;
    unsigned int cap;
    char *out;
};

struct tcpaccept_out {
    uint8_t ip[4];
    uint16_t port;
    uint16_t _pad;
};

/* SYS_TCPLISTEN (Phase 12): start listening on a port (one
 * listener at a time). SYS_TCPACCEPT blocks in 100 ms slices
 * (up to ~30 s) until a handshake completes, driving SYN-ACK
 * retransmits from the wait loop. SYS_TCPSRV_CLOSE FINs the
 * accepted connection, waits (bounded) for the close to finish,
 * then frees the accepted slot and removes the listener. */
static int64_t sys_tcplisten(uint64_t port)
{
    if (port == 0 || port > 65535) return -1;
    return net_tcp_listen((uint16_t)port);
}

static int64_t sys_tcpaccept(uint64_t out_va)
{
    if (!out_va) return -1;
    struct tcpaccept_out *o = (struct tcpaccept_out *)out_va;
    for (int i = 0; i < 300; i++) {
        if (net_tcp_accepted(o->ip, &o->port)) return 0;
        net_tcp_server_tick();
        sched_block_current((uint64_t)timer_ticks() + 10);
    }
    /* Timeout (Phase 12 inc.4, review-001): release the listener
     * and any half-open slot instead of leaking them. */
    net_tcp_server_reset();
    net_tcp_unlisten();
    return -1;
}

static int64_t sys_tcpsrv_close(void)
{
    /* No accepted connection (e.g. it was killed by an RST, or
     * this is the accept-timeout cleanup): still release the
     * listener and any server slot (Phase 12 inc.4, review-001). */
    if (net_tcp_server_close() != 0) {
        net_tcp_server_reset();
        net_tcp_unlisten();
        return -1;
    }
    for (int i = 0; i < 50; i++) {
        if (net_tcp_server_state() == NET_TCP_CLOSED) break;
        net_tcp_server_tick();
        sched_block_current((uint64_t)timer_ticks() + 10);
    }
    net_tcp_server_reset();
    net_tcp_unlisten();
    return 0;
}

/* SYS_TCPSRV_SEND / SYS_TCPSRV_RECV (Phase 12 inc.3): data on
 * the accepted server connection. Send stages through a kernel
 * buffer, waits for the one-segment tx slot, then for the ACK;
 * recv blocks until data, EOF, or a ~60 s bound (0 = EOF or
 * timeout, -1 = no connection). Both drive the server
 * retransmit tick from their wait loops. */
static int64_t sys_tcpsrv_send(uint64_t data_va, uint64_t len)
{
    if (!data_va || len == 0 || len > 1024) return -1;
    static uint8_t skbuf[1024];
    const uint8_t *d = (const uint8_t *)data_va;
    for (uint64_t k = 0; k < len; k++) skbuf[k] = d[k];
    for (int i = 0; i < 50; i++) {
        if (net_tcp_server_send(skbuf, (uint32_t)len) == 0) goto sent;
        if (net_tcp_server_state() == NET_TCP_CLOSED) return -1;
        net_tcp_server_tick();
        sched_block_current((uint64_t)timer_ticks() + 10);
    }
    return -1;
  sent:
    for (int i = 0; i < 50; i++) {
        if (net_tcp_server_acked()) {
            if (net_tcp_server_state() == NET_TCP_CLOSED) return -1;
            return (int64_t)len;
        }
        net_tcp_server_tick();
        sched_block_current((uint64_t)timer_ticks() + 10);
    }
    return -1;
}

static int64_t sys_tcpsrv_recv(uint64_t out_va, uint64_t cap)
{
    if (!out_va || cap == 0 || cap > 2048) return -1;
    uint8_t *out = (uint8_t *)out_va;
    for (int i = 0; i < 600; i++) {
        int n = net_tcp_server_recv(out, (uint32_t)cap);
        if (n > 0) return n;
        if (net_tcp_server_eof()) return 0;
        if (net_tcp_server_state() == NET_TCP_CLOSED) return 0;
        net_tcp_server_tick();
        sched_block_current((uint64_t)timer_ticks() + 10);
    }
    return 0;
}

static int64_t sys_tcpecho(uint64_t ip_va, uint64_t port,
                           uint64_t args_va)
{
    if (!ip_va || !args_va) return -1;
    const uint8_t *ip = (const uint8_t *)ip_va;
    struct tcpecho_args *a = (struct tcpecho_args *)args_va;
    if (!a->data || !a->out || a->len == 0 || a->len > 1024
        || a->cap == 0)
        return -1;
    uint8_t hop[4];
    net_route_ip(ip, hop);
    uint8_t mac[6];
    if (!resolve_mac(hop, mac)) return -1;
    net_tcp_reset();
    if (net_tcp_connect(ip, mac, (uint16_t)port) != 0) return -1;
    int i;
    for (i = 0; i < 50; i++) {
        if (net_tcp_state() == NET_TCP_ESTABLISHED) break;
        if (net_tcp_error()) { net_tcp_reset(); return -1; }
        net_tcp_retransmit();
        sched_block_current((uint64_t)timer_ticks() + 10);
    }
    if (net_tcp_state() != NET_TCP_ESTABLISHED) {
        net_tcp_reset();
        return -1;
    }
    static uint8_t kbuf[1024];
    const char *d = a->data;
    for (unsigned int k = 0; k < a->len; k++)
        kbuf[k] = (uint8_t)d[k];
    if (net_tcp_send(kbuf, a->len) != 0) {
        net_tcp_reset();
        return -1;
    }
    for (i = 0; i < 50; i++) {
        if (net_tcp_acked()) break;
        if (net_tcp_error()) break;
        net_tcp_retransmit();
        sched_block_current((uint64_t)timer_ticks() + 10);
    }
    unsigned int total = 0;
    for (i = 0; i < 100 && total < a->cap; i++) {
        int n = net_tcp_recv((uint8_t *)a->out + total,
                             a->cap - total);
        if (n > 0) total += (unsigned int)n;
        if (net_tcp_eof()) break;
        if (net_tcp_error()) break;
        sched_block_current((uint64_t)timer_ticks() + 10);
    }
    net_tcp_close();
    for (i = 0; i < 20; i++) {
        if (net_tcp_state() == NET_TCP_CLOSED) break;
        net_tcp_retransmit();
        sched_block_current((uint64_t)timer_ticks() + 5);
    }
    int err = net_tcp_error();
    net_tcp_reset();
    if (err && total == 0) return -1;
    return (int64_t)total;
}

/* SYS_BCACHE_SYNC: flush all dirty cache entries to disk. */
static int64_t sys_bcache_sync(void)
{
    return bcache_sync();
}

/* SYS_DMA_STAT: dma_stat(dma_va, err_va, pio_va)
 * Writes ATA DMA/PIO transfer counters to userspace. Returns 0 or -1. */
static int64_t sys_dma_stat(uint64_t dma_va, uint64_t err_va, uint64_t pio_va)
{
    if (!dma_va || !err_va || !pio_va)
        return -1;
    uint64_t d, e, p;
    ata_dma_stats(&d, &e, &p);
    *(uint64_t *)dma_va = d;
    *(uint64_t *)err_va = e;
    *(uint64_t *)pio_va = p;
    return 0;
}

/* SYS_WSTAT: wstat(dma_va, err_va, pio_va)
 * Writes ATA write-path counters to userspace. Returns 0 or -1. */
static int64_t sys_wstat(uint64_t dma_va, uint64_t err_va, uint64_t pio_va)
{
    if (!dma_va || !err_va || !pio_va)
        return -1;
    uint64_t d, e, p;
    ata_write_stats(&d, &e, &p);
    *(uint64_t *)dma_va = d;
    *(uint64_t *)err_va = e;
    *(uint64_t *)pio_va = p;
    return 0;
}

/* SYS_RAW_SECTOR: raw_sector(idx, buf_va, is_write) — driver test
 * hook (Phase 6). Reads or writes ONE sector in a scratch area at
 * the very end of the disk (the last 2048 sectors), far beyond the
 * MyFS image, so tests can exercise ata_read/ata_write (later the
 * block cache) without endangering filesystem data. Returns 0/-1. */
#define RAW_SCRATCH_SECTORS 2048
static int64_t sys_raw_sector(uint64_t idx, uint64_t buf_va, uint64_t is_write)
{
    uint32_t total = ata_total_sectors();
    if (!buf_va || total < 8192 || idx >= RAW_SCRATCH_SECTORS)
        return -1;
    uint32_t lba = total - RAW_SCRATCH_SECTORS + (uint32_t)idx;
    /* Phase 6 inc.2: route through the block cache, so the hook
     * exercises bcache_write (write-back) / bcache_read. */
    if (is_write)
        return bcache_write(lba, (const uint8_t *)buf_va);
    return bcache_read(lba, (uint8_t *)buf_va);
}

/* SYS_PREFETCH_POLICY: prefetch_policy(index) -> 0 or -1.
 * Index 0 = none, 1 = sequential, 2 = stride, 3 = learned,
 * 4 = learned-online (Phase 10: weights update at runtime). */
static int64_t sys_prefetch_policy(uint64_t index)
{
    if (index == 0) { prefetch_set_policy(&prefetch_none); return 0; }
    if (index == 1) { prefetch_set_policy(&prefetch_sequential); return 0; }
    if (index == 2) { prefetch_set_policy(&prefetch_stride); return 0; }
    if (index == 3) { prefetch_set_policy(&prefetch_learned); return 0; }
    if (index == 4) { prefetch_set_policy(&prefetch_learned_online); return 0; }
    return -1;
}

/* SYS_PFWEIGHTS (Phase 10): inspect or reset the learned
 * prefetch weights. mode 0 copies {int32 w[13]; uint32 pos, neg}
 * to the caller; mode 1 zeroes the weights; mode 2 reseeds them
 * from the trained values; mode 3 saves them to pfweights.bin
 * on disk (inc.3). */
struct pfweights_out {
    int32_t w[13];
    uint32_t pos;
    uint32_t neg;
};

static int64_t sys_pfweights(uint64_t mode, uint64_t out_va)
{
    if (mode == 0) {
        if (!out_va) return -1;
        struct pfweights_out *o = (struct pfweights_out *)out_va;
        prefetch_weights_read(o->w, &o->pos, &o->neg);
        return 0;
    }
    if (mode == 1) { prefetch_weights_zero(); return 0; }
    if (mode == 2) { prefetch_weights_seed(); return 0; }
    if (mode == 3) { return prefetch_weights_save(); }
    return -1;
}

/* SYS_BCACHE_PROBE (Phase 13 inc.8): cache-state probe for
 * tests: bit0 valid, bit1 dirty, bit2 prefetched-unused. */
static int64_t sys_bcache_probe(uint64_t lba)
{
    return bcache_probe((uint32_t)lba);
}

/* SYS_DF (Phase 13 inc.5): MyFS capacity reporting for the
 * shell `df` command. The out struct mirrors struct diskfs_df
 * in diskfs.h field for field (all uint32_t). */
struct df_out {
    uint32_t cap_sectors, free_sectors, records, live_records,
             live_sectors, dead_records, dead_sectors;
};

static int64_t sys_df(uint64_t out_va)
{
    if (!out_va) return -1;
    return diskfs_df((struct diskfs_df *)out_va);
}

/* SYS_FUNLINK / SYS_FRENAME (Phase 11): delete and rename
 * disk files via the VFS. */
static int64_t sys_funlink(uint64_t path_va)
{
    if (!path_va) return -1;
    char kpath[64];
    if (copy_string_from_user(kpath, path_va, sizeof kpath) < 0)
        return EFAULT;
    return vfs_unlink(kpath);
}

static int64_t sys_frename(uint64_t old_va, uint64_t new_va)
{
    if (!old_va || !new_va) return -1;
    char kold[64], knew[64];
    if (copy_string_from_user(kold, old_va, sizeof kold) < 0)
        return EFAULT;
    if (copy_string_from_user(knew, new_va, sizeof knew) < 0)
        return EFAULT;
    return vfs_rename(kold, knew);
}

/* SYS_ACCESS_DRAIN: access_drain(buf_va, max_records)
 * Copies up to max_records demand-access records (oldest first) into
 * the caller's buffer. Returns the number of records written, or -1. */
static int64_t sys_access_drain(uint64_t buf_va, uint64_t max_records)
{
    if (!buf_va || max_records == 0 || max_records > 4096)
        return -1;
    struct access_record *buf = (struct access_record *)buf_va;
    return (int64_t)access_trace_drain(buf, (int)max_records);
}

/* -----------------------------------------------------------------------
 * Dispatch table
 * ----------------------------------------------------------------------- */

int64_t syscall_dispatch(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    /* Count every syscall against the calling task (excluding SYS_EXIT
     * since the task is already tearing down). */
    if (nr != SYS_EXIT) {
        struct task *ct = sched_current_task();
        if (ct) ct->syscall_count++;
    }

    switch (nr) {
    case SYS_WRITE:   return sys_write(arg1, arg2, arg3);
    case SYS_READ:    return sys_read(arg1, arg2, arg3);
    case SYS_EXIT:    return sys_exit(arg1);
    case SYS_SBRK:    return sys_sbrk(arg1);
    case SYS_CLEAR:   return sys_clear();
    case SYS_GETPID:  return sys_getpid();
    case SYS_WAIT:    return sys_wait(arg1);
    case SYS_EXEC:    return sys_exec(arg1, arg2);
    case SYS_EXEC_INHERIT: return sys_exec_inherit(arg1, arg2, arg3);
    case SYS_OPEN:    return sys_open(arg1);
    case SYS_FCREATE: return sys_fcreate(arg1);
    case SYS_NETSTAT: return sys_netstat(arg1);
    case SYS_NETSEND: return sys_netsend();
    case SYS_PING: return sys_ping(arg1, arg2);
    case SYS_DHCP: return sys_dhcp(arg1);
    case SYS_NSLOOKUP: return sys_nslookup(arg1, arg2, arg3);
    case SYS_TCPECHO: return sys_tcpecho(arg1, arg2, arg3);
    case SYS_TCPLISTEN: return sys_tcplisten(arg1);
    case SYS_TCPACCEPT: return sys_tcpaccept(arg1);
    case SYS_TCPSRV_CLOSE: return sys_tcpsrv_close();
    case SYS_TCPSRV_SEND: return sys_tcpsrv_send(arg1, arg2);
    case SYS_TCPSRV_RECV: return sys_tcpsrv_recv(arg1, arg2);
    case SYS_DF: return sys_df(arg1);
    case SYS_BCACHE_PROBE: return sys_bcache_probe(arg1);
    case SYS_DUP2:    return sys_dup2(arg1, arg2);
    case SYS_PIPE:    return sys_pipe(arg1);
    case SYS_KILL:    return sys_kill(arg1, arg2);
    case SYS_GETPGID: return sys_getpgid(arg1);
    case SYS_SETPGID: return sys_setpgid(arg1, arg2);
    case SYS_SETFG:   return sys_setfg(arg1);
    case SYS_TRYWAIT: return sys_trywait(arg1);
    case SYS_CLOSE:   return sys_close(arg1);
    case SYS_READDIR: return sys_readdir(arg1, arg2);
    case SYS_TIME:    return sys_time(arg1);
    case SYS_TELEMETRY: return sys_telemetry(arg1, arg2);
    case SYS_SETPOLICY: return sys_setpolicy(arg1);
    case SYS_TELEMETRY_DRAIN: return sys_telemetry_drain(arg1, arg2);
    case SYS_PF_DRAIN: return sys_pf_drain(arg1, arg2);
    case SYS_PF_SETPOLICY: return sys_pf_setpolicy(arg1);
    case SYS_IO_DRAIN: return sys_io_drain(arg1, arg2);
    case SYS_BCACHE_STAT: return sys_bcache_stat(arg1, arg2, arg3);
    case SYS_BCACHE_SYNC: return sys_bcache_sync();
    case SYS_DMA_STAT: return sys_dma_stat(arg1, arg2, arg3);
    case SYS_WSTAT: return sys_wstat(arg1, arg2, arg3);
    case SYS_RAW_SECTOR: return sys_raw_sector(arg1, arg2, arg3);
    case SYS_PREFETCH_POLICY: return sys_prefetch_policy(arg1);
    case SYS_PFWEIGHTS: return sys_pfweights(arg1, arg2);
    case SYS_FUNLINK: return sys_funlink(arg1);
    case SYS_FRENAME: return sys_frename(arg1, arg2);
    case SYS_ACCESS_DRAIN: return sys_access_drain(arg1, arg2);
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
