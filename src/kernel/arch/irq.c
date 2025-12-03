#include <stdint.h>
#include "idt.h"
#include "pic.h"

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
