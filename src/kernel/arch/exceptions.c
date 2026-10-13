#include <stdint.h>
#include "exceptions.h"
#include "idt.h"
#include "port_io.h"
#include "../mem/vmm.h"
#include "../mem/pmm.h"
#include "timer.h"
#include "../proc/sched.h"

extern void pf_stub(void);

/* -----------------------------------------------------------------------
 * Demand-paged user region (Phase 2, increment 2).
 *
 * Pages in [DEMAND_BASE, DEMAND_BASE + DEMAND_PAGES*4K) are never mapped
 * up front. The first touch of each page raises #PF (not-present, from
 * ring 3); the handler allocates a zeroed frame and maps it, then
 * returns so the faulting instruction re-executes and succeeds.
 *
 * DEMAND_BASE = 0x20000000000 (2TB): clear of user ELFs (1TB),
 * the user stack (1TB+16MB), and the inc.1 probe address (5TB).
 * ----------------------------------------------------------------------- */
#define DEMAND_BASE  0x20000000000UL
#define DEMAND_PAGES 16UL

/* COM1 was initialised by kernel_main's serial_init(); reuse the port. */
static void exc_putchar(char c)
{
    while (!(inb(0x3F8 + 5) & 0x20)) { }
    outb(0x3F8, (uint8_t)c);
}

static void exc_puts(const char *s)
{
    while (*s) exc_putchar(*s++);
}

static void exc_puthex64(uint64_t v)
{
    exc_puts("0x");
    for (int i = 15; i >= 0; i--) {
        uint8_t n = (v >> (i * 4)) & 0xF;
        exc_putchar(n < 10 ? (char)('0' + n) : (char)('A' + n - 10));
    }
}

static void zero_frame(uint64_t phys)
{
    /* PMM frames live in the identity-mapped region: phys == virt. */
    uint8_t *p = (uint8_t *)phys;
    for (uint64_t i = 0; i < PAGE_SIZE; i++)
        p[i] = 0;
}

/*
 * Page-fault handler (Phase 2, increment 2: demand paging).
 *
 * Prints the faulting address (CR2) and the decoded error code. If the
 * fault is a not-present access from ring 3 inside the demand region,
 * it allocates a zeroed frame, maps it into the *faulting* address
 * space (the interrupt did not switch CR3, so vmm_current_cr3() is the
 * faulting process's), and returns -- pf_stub's iretq re-executes
 * the faulting instruction, which now succeeds. Anything else
 * (protection fault, supervisor fault, address outside the demand
 * region) still halts, exactly as in increment 1.
 */
/* -----------------------------------------------------------------------
 * Fault telemetry ring (Phase 2, increment 3).
 *
 * Records every #PF: tick, CR2, error code, faulting pid, and whether
 * the handler resolved it via demand-mapping. Drained by SYS_PF_DRAIN.
 * ----------------------------------------------------------------------- */
static struct pf_record pf_ring[PF_TRACE_RING_SIZE];
static uint32_t pf_head  = 0;   /* index of oldest record */
static uint32_t pf_count = 0;   /* number of valid records */

static void pf_trace_record(uint64_t cr2, uint64_t err_code, int resolved)
{
    uint32_t pid = 0;
    if (err_code & 0x4) {  /* user-mode fault: attribute to current task */
        struct task *t = sched_current_task();
        if (t) pid = t->pid;
    }

    __asm__ volatile("cli");
    uint32_t slot;
    if (pf_count < PF_TRACE_RING_SIZE) {
        slot = (pf_head + pf_count) % PF_TRACE_RING_SIZE;
        pf_count++;
    } else {
        /* Ring full: overwrite oldest. */
        slot = pf_head;
        pf_head = (pf_head + 1) % PF_TRACE_RING_SIZE;
    }
    pf_ring[slot].tick      = (uint64_t)timer_ticks();
    pf_ring[slot].cr2       = cr2;
    pf_ring[slot].err_code  = err_code;
    pf_ring[slot].pid       = pid;
    pf_ring[slot].resolved  = (uint32_t)resolved;
    __asm__ volatile("sti");
}

int pf_trace_drain(struct pf_record *buf, int max)
{
    if (!buf || max <= 0)
        return 0;

    __asm__ volatile("cli");
    uint32_t n = pf_count < (uint32_t)max ? pf_count : (uint32_t)max;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = (pf_head + i) % PF_TRACE_RING_SIZE;
        buf[i] = pf_ring[slot];
    }
    pf_head   = (pf_head + n) % PF_TRACE_RING_SIZE;
    pf_count -= n;
    __asm__ volatile("sti");

    return (int)n;
}

void page_fault_handler(uint64_t err_code)
{
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    /* Telemetry: record before deciding. Resolved iff we will demand-map. */
    uint64_t page0 = cr2 & ~(PAGE_SIZE - 1);
    int will_resolve = (!(err_code & 0x1)) && (err_code & 0x4) &&
                     (page0 >= DEMAND_BASE) &&
                     (page0 < DEMAND_BASE + DEMAND_PAGES * PAGE_SIZE);
    pf_trace_record(cr2, err_code, will_resolve);

    exc_puts("\r\n[pf] PAGE FAULT cr2=");
    exc_puthex64(cr2);
    exc_puts(" err=");
    exc_puthex64(err_code);
    exc_puts(" (");
    exc_puts((err_code & 0x1) ? "protection" : "not-present");
    exc_puts((err_code & 0x2) ? " write" : " read");
    exc_puts((err_code & 0x4) ? " user" : " supervisor");
    if (err_code & 0x8)  exc_puts(" reserved-bit");
    if (err_code & 0x10) exc_puts(" instr-fetch");
    exc_puts(")\r\n");

    uint64_t page = cr2 & ~(PAGE_SIZE - 1);
    int not_present = !(err_code & 0x1);
    int user_mode   = (err_code & 0x4) != 0;
    int in_region   = (page >= DEMAND_BASE) &&
                    (page < DEMAND_BASE + DEMAND_PAGES * PAGE_SIZE);

    if (not_present && user_mode && in_region) {
        uint64_t cr3   = vmm_current_cr3();
        uint64_t frame = pmm_alloc_page();
        if (!frame) {
            exc_puts("[pf] demand-map OOM - halting\r\n");
            goto halt;
        }
        zero_frame(frame);
        vmm_map_page_in(cr3, frame, page,
                        PAGE_PRESENT | PAGE_RW | PAGE_USER);
        exc_puts("[pf] demand-mapped page ");
        exc_puthex64(page);
        exc_puts(" <- frame ");
        exc_puthex64(frame);
        exc_puts("\r\n");
        /* Return: iretq re-executes the faulting instruction. */
        return;
    }

    exc_puts("[pf] outside demand region or unrecoverable - halting\r\n");

halt:
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

void exceptions_init(void)
{
    /* Vector 14 = #PF. 0x8E = present, ring 0, 64-bit interrupt gate. */
    idt_set_gate(14, (uint64_t)pf_stub, 0x08, 0x8E);
}
