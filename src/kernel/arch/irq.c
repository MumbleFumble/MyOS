#include <stdint.h>
#include "idt.h"
#include "pic.h"

/* External IRQ stub handlers from irq_stubs.S */
extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);

static void default_irq_handler(void);

static void (*irq_handlers[16])(void) = {
    default_irq_handler, default_irq_handler, default_irq_handler, default_irq_handler,
    default_irq_handler, default_irq_handler, default_irq_handler, default_irq_handler,
    default_irq_handler, default_irq_handler, default_irq_handler, default_irq_handler,
    default_irq_handler, default_irq_handler, default_irq_handler, default_irq_handler
};

void irq_install_handler(int irq, void (*handler)(void))
{
    if (irq < 0 || irq >= 16) return;
    irq_handlers[irq] = handler ? handler : default_irq_handler;
}

void irq_dispatch(int irq)
{
    if (irq < 0 || irq >= 16) return;
    irq_handlers[irq]();
    pic_send_eoi((uint8_t)irq);
}

static void default_irq_handler(void)
{
    /* do nothing */
}

void irq_init(void)
{
    /* Remap PIC: IRQ 0-15 -> IDT entries 32-47 */
    pic_remap(32, 40);
    
    /* Install IRQ handlers in IDT (0x8E = present, ring 0, interrupt gate) */
    idt_set_gate(32, (uint64_t)irq0, 0x08, 0x8E);
    idt_set_gate(33, (uint64_t)irq1, 0x08, 0x8E);
    idt_set_gate(34, (uint64_t)irq2, 0x08, 0x8E);
    idt_set_gate(35, (uint64_t)irq3, 0x08, 0x8E);
    idt_set_gate(36, (uint64_t)irq4, 0x08, 0x8E);
    idt_set_gate(37, (uint64_t)irq5, 0x08, 0x8E);
    idt_set_gate(38, (uint64_t)irq6, 0x08, 0x8E);
    idt_set_gate(39, (uint64_t)irq7, 0x08, 0x8E);
    idt_set_gate(40, (uint64_t)irq8, 0x08, 0x8E);
    idt_set_gate(41, (uint64_t)irq9, 0x08, 0x8E);
    idt_set_gate(42, (uint64_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint64_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint64_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint64_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint64_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint64_t)irq15, 0x08, 0x8E);
    
    /* Enable interrupts */
    __asm__ volatile("sti");
}
