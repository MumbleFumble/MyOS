#include "syscall.h"
#include "../arch/idt.h"
#include "../arch/gdt.h"
#include "../arch/port_io.h"
#include "../proc/sched.h"
#include "../proc/policy_wf.h"
#include "../proc/policy_ml.h"
#include "../arch/exceptions.h"
#include "../proc/elf.h"
#include "../fs/vfs.h"
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

/* SYS_WRITE: write(fd, buf, count) */
static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count)
{
    /* File fd (Phase 6 inc.3): route to the VFS write op. */
    if (fd >= 3 && fd < MAX_FDS) {
        struct task *t = sched_current_task();
        struct vfs_node *node = (struct vfs_node *)t->fds[fd].node;
        if (!node) return -1;
        int64_t w = vfs_write(node, t->fds[fd].offset, count,
                              (uint8_t *)buf);
        if (w > 0) t->fds[fd].offset += (uint64_t)w;
        return w;
    }
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

static int64_t fd_alloc(struct task *t, vfs_node_t *node)
{
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

/* SYS_OPEN: open(path) -> fd, or -1 on not-found / table full */
static int64_t sys_open(uint64_t path_va)
{
    const char *path = (const char *)path_va;
    vfs_node_t *node = vfs_open(path);
    if (!node) return -1;
    return fd_alloc(sched_current_task(), node);
}

/* SYS_FCREATE: fcreate(path) -> fd (Phase 6 inc.3). Creates the
 * file if needed, truncates it if it exists. */
static int64_t sys_fcreate(uint64_t path_va)
{
    const char *path = (const char *)path_va;
    vfs_node_t *node = vfs_create(path);
    if (!node) return -1;
    return fd_alloc(sched_current_task(), node);
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
                    &o->udp_rx, &o->udp_tx);
    o->irq_rx = e1000_rx_irqs();
    return 0;
}

/* SYS_NETSEND (Phase 7): send the fixed test frame. */
static int64_t sys_netsend(void)
{
    return e1000_send_hello();
}

/* SYS_PING (Phase 7): resolve ARP if needed, send an ICMP echo
 * request and block (in 100 ms slices) for the reply. Returns
 * the round-trip time in timer ticks (10 ms each) or -1. */
static int64_t sys_ping(uint64_t ip_va, uint64_t seq)
{
    if (!ip_va) return -1;
    const uint8_t *ip = (const uint8_t *)ip_va;
    uint8_t mac[6];
    if (!net_arp_lookup(ip, mac)) {
        net_arp_request(ip);
        int found = 0;
        for (int i = 0; i < 30 && !found; i++) {
            sched_block_current((uint64_t)timer_ticks() + 10);
            found = net_arp_lookup(ip, mac);
        }
        if (!found) return -1;
    }
    if (net_ping_send(ip, mac, (uint16_t)seq) != 0) return -1;
    for (int i = 0; i < 30; i++) {
        uint32_t rtt = 0;
        if (net_ping_result(&rtt) == 1) return (int64_t)rtt;
        sched_block_current((uint64_t)timer_ticks() + 10);
    }
    return -1;
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
 * Index 0 = none, 1 = sequential, 2 = stride, 3 = learned. */
static int64_t sys_prefetch_policy(uint64_t index)
{
    if (index == 0) { prefetch_set_policy(&prefetch_none); return 0; }
    if (index == 1) { prefetch_set_policy(&prefetch_sequential); return 0; }
    if (index == 2) { prefetch_set_policy(&prefetch_stride); return 0; }
    if (index == 3) { prefetch_set_policy(&prefetch_learned); return 0; }
    return -1;
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
    case SYS_OPEN:    return sys_open(arg1);
    case SYS_FCREATE: return sys_fcreate(arg1);
    case SYS_NETSTAT: return sys_netstat(arg1);
    case SYS_NETSEND: return sys_netsend();
    case SYS_PING: return sys_ping(arg1, arg2);
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
