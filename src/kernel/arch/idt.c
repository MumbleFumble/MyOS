#include "idt.h"

#define IDT_ENTRIES 256

static struct idt_entry idt[IDT_ENTRIES];
static struct idtr idtr_desc;

extern void idt_load(struct idtr*);

void idt_set_gate(int n, uint64_t handler, uint16_t sel, uint8_t flags)
{
    idt[n].offset_low  = handler & 0xFFFF;
    idt[n].selector    = sel;
    idt[n].ist         = 0;
    idt[n].type_attr   = flags;
    idt[n].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[n].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[n].zero        = 0;
}

void idt_init(void)
{
    for (int i = 0; i < IDT_ENTRIES; ++i)
    {
        idt[i].offset_low  = 0;
        idt[i].selector    = 0;
        idt[i].ist         = 0;
        idt[i].type_attr   = 0;
        idt[i].offset_mid  = 0;
        idt[i].offset_high = 0;
        idt[i].zero        = 0;
    }

    idtr_desc.limit = sizeof(idt) - 1;
    idtr_desc.base  = (uint64_t)&idt;

    idt_load(&idtr_desc);
}
