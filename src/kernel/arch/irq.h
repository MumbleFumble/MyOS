#pragma once

#include <stdint.h>
#include "idt.h"

typedef void (*irq_handler_t)(void);

void irq_install_handler(int irq, irq_handler_t handler);
void irq_dispatch(int irq);
