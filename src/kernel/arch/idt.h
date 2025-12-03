#pragma once

#include <stdint.h>

struct idt_entry {
    uint16_t offset_low;    /* Offset bits 0-15 */
    uint16_t selector;      /* Code segment selector */
    uint8_t  ist;           /* Interrupt Stack Table offset (bits 0-2), rest reserved */
    uint8_t  type_attr;     /* Type and attributes */
    uint16_t offset_mid;    /* Offset bits 16-31 */
    uint32_t offset_high;   /* Offset bits 32-63 */
    uint32_t zero;          /* Reserved, must be 0 */
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void idt_init(void);
void idt_set_gate(int n, uint64_t handler, uint16_t sel, uint8_t flags);

/* Simple hook for IRQ handlers */
typedef void (*irq_handler_t)(void);
void irq_install_handler(int irq, irq_handler_t handler);
