#include <stdint.h>
#include "exceptions.h"
#include "idt.h"
#include "port_io.h"
#include "../mem/vmm.h"
#include "../mem/pmm.h"

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
void page_fault_handler(uint64_t err_code)
{
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

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
