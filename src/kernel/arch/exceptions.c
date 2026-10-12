#include <stdint.h>
#include "exceptions.h"
#include "idt.h"
#include "port_io.h"

extern void pf_stub(void);

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

/*
 * Page-fault handler (Phase 2, increment 1: observe only).
 *
 * Prints the faulting address (CR2) and the decoded error code, then
 * halts. Returning to the faulting instruction is not safe yet: with no
 * demand pager the same fault would just fire again. Increment 2 will
 * map the page and resume instead of halting.
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
    exc_puts("[pf] no demand pager yet (phase 2 inc.1) - halting\r\n");

    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

void exceptions_init(void)
{
    /* Vector 14 = #PF. 0x8E = present, ring 0, 64-bit interrupt gate. */
    idt_set_gate(14, (uint64_t)pf_stub, 0x08, 0x8E);
}
