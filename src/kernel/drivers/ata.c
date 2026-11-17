#include "ata.h"
#include "../arch/port_io.h"
#include "../arch/timer.h"
#include "../arch/irq.h"
#include "../proc/sched.h"

/* Primary ATA channel I/O ports */
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECT_COUNT  0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7

/* Status register bits */
#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

/* Commands */
#define ATA_CMD_READ_PIO 0x20

static volatile int ata_irq_flag = 0;

static void ata_delay400ns(void)
{
    /* Read the alternate status port 4 times = ~400 ns delay */
    inb(0x3F6); inb(0x3F6); inb(0x3F6); inb(0x3F6);
}

static int ata_wait_not_busy(void)
{
    int timeout = 100000;
    while ((inb(ATA_STATUS) & ATA_SR_BSY) && --timeout);
    return timeout > 0 ? 0 : -1;
}

static int ata_wait_drq(void)
{
    int timeout = 100000;
    uint8_t status;
    do {
        status = inb(ATA_STATUS);
        /* 0xFF is a transient float seen just after command issue (and
         * around stale interrupts), not a drive error: keep waiting.
         * Genuine errors come with DRDY set. */
        if (status != 0xFF && (status & ATA_SR_ERR)) return -1;
        if (status & ATA_SR_DRQ) return 0;
    } while (--timeout);
    return -1;
}

/* -----------------------------------------------------------------------
 * IRQ-driven sector waits (Phase 4, increment 1).
 *
 * With IRQ mode on, a task waiting for DRQ blocks (TASK_BLOCKED) until
 * the IRQ14 handler wakes it, instead of spinning on the status port.
 * Safety nets, in order: the scheduler wakes blocked tasks at a
 * deadline (50 ticks) even if the interrupt is lost, and any doubt
 * falls back to the bounded polling wait above. The idle task never
 * blocks (sched_block_current_io refuses), so early-boot and mount
 * reads keep polling.
 * ----------------------------------------------------------------------- */
static struct task *ata_waiter  = 0;    /* task blocked on DRQ, if any */
static int ata_irq_mode = 0;

/* -----------------------------------------------------------------------
 * Disk mutex (Phase 4, increment 2).
 *
 * With the async prefetch worker there are now two concurrent readers
 * (demand tasks and the worker), but the drive has one command channel
 * and ata_waiter is a single slot: ata_read must be serialised. This is
 * a sleep mutex with ownership handoff: unlock transfers ownership
 * directly to the first waiter and wakes it. The wait deadline (200
 * ticks) is only a backstop against a lost wake. Before IRQ mode is
 * enabled (early boot, single-threaded) the lock is skipped entirely,
 * so the idle task never has to wait for it.
 * ----------------------------------------------------------------------- */
#define ATA_LOCK_MAX_WAITERS 4
static struct task *ata_lock_owner = 0;
static struct task *ata_lock_waiters[ATA_LOCK_MAX_WAITERS];
static int ata_lock_nwait = 0;

void ata_lock(void)
{
    if (!ata_irq_mode)
        return;
    struct task *self = sched_current_task();
    for (;;) {
        uint64_t rflags;
        __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
        __asm__ volatile("cli");
        /* Drop any stale registration of ours (deadline-wake retry):
         * being registered twice would strand ownership on us later. */
        for (int i = 0; i < ata_lock_nwait; i++) {
            if (ata_lock_waiters[i] == self) {
                for (int j = i + 1; j < ata_lock_nwait; j++)
                    ata_lock_waiters[j - 1] = ata_lock_waiters[j];
                ata_lock_nwait--;
                break;
            }
        }
        if (!ata_lock_owner) {
            ata_lock_owner = self;
            if (rflags & 0x200) __asm__ volatile("sti");
            return;
        }
        if (ata_lock_owner == self) {   /* re-entrant call: should not happen */
            if (rflags & 0x200) __asm__ volatile("sti");
            return;
        }
        if (ata_lock_nwait < ATA_LOCK_MAX_WAITERS)
            ata_lock_waiters[ata_lock_nwait++] = self;
        if (rflags & 0x200) __asm__ volatile("sti");
        /* Sleep until unlock hands ownership to us (or the backstop
         * deadline passes, in which case we loop and re-check). */
        if (!sched_block_current((uint64_t)timer_ticks() + 200)) {
            /* Idle task cannot block: spin. Only reachable if IRQ mode
             * was enabled while a boot-context read races a task read;
             * the holder always finishes, so this terminates. */
            continue;
        }
        if (ata_lock_owner == self)
            return;
        /* Woken without ownership (deadline): loop and re-register. */
    }
}

void ata_unlock(void)
{
    if (!ata_irq_mode)
        return;
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    __asm__ volatile("cli");
    if (ata_lock_nwait > 0) {
        /* Handoff: first waiter becomes owner before being woken, so
         * no third task can slip in between. */
        struct task *next = ata_lock_waiters[0];
        for (int i = 1; i < ata_lock_nwait; i++)
            ata_lock_waiters[i - 1] = ata_lock_waiters[i];
        ata_lock_nwait--;
        ata_lock_owner = next;
        sched_wake_io(next);
        sched_request_resched();
    } else {
        ata_lock_owner = 0;
    }
    if (rflags & 0x200) __asm__ volatile("sti");
}

static void ata_irq_handler(void)
{
    (void)inb(ATA_STATUS);   /* reading status clears INTRQ */
    ata_irq_flag = 1;
    if (ata_waiter && ata_waiter->state == TASK_BLOCKED) {
        sched_wake_io(ata_waiter);
        /* Never call sched_tick from here: it is not reentrant, and a
         * switch nested inside IRQ/syscall frames corrupts saved stack
         * pointers. The idle loop notices the request and reschedules
         * from its top level within microseconds (hlt wakes on IRQ). */
        sched_request_resched();
    }
}

void ata_enable_irq(void)
{
    irq_install_handler(14, ata_irq_handler);
    /* Unmask the cascade IRQ2 (master PIC) and IRQ14 (slave PIC). */
    outb(0x21, (uint8_t)(inb(0x21) & ~(1 << 2)));
    outb(0xA1, (uint8_t)(inb(0xA1) & ~(1 << 6)));
    ata_irq_mode = 1;
}

/* Wait for DRQ for one sector, sleeping on IRQ14 when possible. */
static int ata_wait_drq_sector(void)
{
    uint8_t st = inb(ATA_STATUS);
    if (st != 0xFF && (st & ATA_SR_ERR)) return -1;
    if (st & ATA_SR_DRQ) return 0;
    if (!ata_irq_mode)
        return ata_wait_drq();

    /* cli across the check-and-block sequence closes the lost-wake
     * race: the IRQ cannot fire between the status/flag check and the
     * state becoming TASK_BLOCKED, and if it fires after the sti in
     * sched_block_current_io but before the switch, the wake simply
     * marks us READY and the switch treats us normally. */
    __asm__ volatile("cli");
    st = inb(ATA_STATUS);
    if (st != 0xFF && (st & ATA_SR_ERR)) { __asm__ volatile("sti"); return -1; }
    if (st & ATA_SR_DRQ) { __asm__ volatile("sti"); return 0; }
    if (ata_irq_flag) {
        /* The interrupt already arrived; DRQ should be imminent. */
        __asm__ volatile("sti");
        return ata_wait_drq();
    }
    ata_waiter = sched_current_task();
    int blocked = sched_block_current_io((uint64_t)timer_ticks() + 50);
    if (!blocked) {
        /* Idle task: blocking refused, interrupts still off. */
        __asm__ volatile("sti");
        return ata_wait_drq();
    }
    /* Resumed via IRQ wake or deadline: re-check, else bounded poll. */
    ata_irq_flag = 0;
    st = inb(ATA_STATUS);
    if (st != 0xFF && (st & ATA_SR_ERR)) return -1;
    if (st & ATA_SR_DRQ) return 0;
    return ata_wait_drq();
}

int ata_detect(void)
{
    /* Select drive 0 (master) with LBA mode */
    outb(ATA_DRIVE_HEAD, 0xE0);
    ata_delay400ns();

    /* If status is 0xFF the bus is floating (no drive) */
    uint8_t status = inb(ATA_STATUS);
    if (status == 0xFF) return 0;

    /* Wait for BSY to clear */
    if (ata_wait_not_busy() != 0) return 0;
    return 1;
}

/* Forward declaration: defined below ata_read(). */
static void io_trace_record(uint32_t lba, uint32_t count,
                            uint64_t tsc_start, uint64_t tsc_end,
                            int result);
static int ata_read_inner(uint32_t lba, uint32_t count, uint8_t *buf);

int ata_read_locked(uint32_t lba, uint32_t count, uint8_t *buf)
{
    uint64_t tsc_start = timer_tsc();
    int result = ata_read_inner(lba, count, buf);
    uint64_t tsc_end = timer_tsc();

    ata_waiter = 0;
    io_trace_record(lba, count, tsc_start, tsc_end, result);
    return result;
}

int ata_read(uint32_t lba, uint32_t count, uint8_t *buf)
{
    ata_lock();
    int result = ata_read_locked(lba, count, buf);
    ata_unlock();
    return result;
}

/* -----------------------------------------------------------------------
 * I/O telemetry ring (Phase 3, increment 1).
 *
 * Records every ata_read(): start/end ticks, LBA, sector count, latency,
 * and result. Drained by SYS_IO_DRAIN.
 * ----------------------------------------------------------------------- */
static struct io_record io_ring[IO_TRACE_RING_SIZE];
static uint32_t io_head  = 0;   /* index of oldest record */
static uint32_t io_count = 0;   /* number of valid records */

static void io_trace_record(uint32_t lba, uint32_t count,
                            uint64_t tsc_start, uint64_t tsc_end,
                            int result)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    __asm__ volatile("cli");
    uint32_t slot;
    if (io_count < IO_TRACE_RING_SIZE) {
        slot = (io_head + io_count) % IO_TRACE_RING_SIZE;
        io_count++;
    } else {
        /* Ring full: overwrite oldest. */
        slot = io_head;
        io_head = (io_head + 1) % IO_TRACE_RING_SIZE;
    }
    io_ring[slot].tsc_start  = tsc_start;
    io_ring[slot].tsc_end    = tsc_end;
    io_ring[slot].lba        = lba;
    io_ring[slot].count      = count;
    /* Convert TSC delta to microseconds (freq calibrated at boot). */
    uint64_t freq = timer_tsc_freq();
    io_ring[slot].latency_us = freq ? (uint32_t)((tsc_end - tsc_start) * 1000000ULL / freq) : 0;
    io_ring[slot].result     = (int32_t)result;
    if (rflags & 0x200) __asm__ volatile("sti");
}

int io_trace_drain(struct io_record *buf, int max)
{
    if (!buf || max <= 0)
        return 0;

    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    __asm__ volatile("cli");
    uint32_t n = io_count < (uint32_t)max ? io_count : (uint32_t)max;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = (io_head + i) % IO_TRACE_RING_SIZE;
        buf[i] = io_ring[slot];
    }
    io_head   = (io_head + n) % IO_TRACE_RING_SIZE;
    io_count -= n;
    if (rflags & 0x200) __asm__ volatile("sti");

    return (int)n;
}

static int ata_read_inner(uint32_t lba, uint32_t count, uint8_t *buf)
{
    if (count == 0) return 0;

    if (ata_wait_not_busy() != 0) return -1;

    /* Select drive 0, LBA mode, upper 4 bits of LBA */
    outb(ATA_DRIVE_HEAD, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_delay400ns();

    outb(ATA_SECT_COUNT, (uint8_t)count);
    outb(ATA_LBA_LO,     (uint8_t)(lba        & 0xFF));
    outb(ATA_LBA_MID,    (uint8_t)((lba >> 8)  & 0xFF));
    outb(ATA_LBA_HI,     (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND,    ATA_CMD_READ_PIO);
    ata_irq_flag = 0;

    for (uint32_t s = 0; s < count; s++) {
        if (ata_wait_drq_sector() != 0) return -1;

        /* Read 256 16-bit words = 512 bytes */
        uint16_t *dst = (uint16_t *)(buf + s * 512);
        for (int i = 0; i < 256; i++)
            dst[i] = inw(ATA_DATA);
    }
    return 0;
}
