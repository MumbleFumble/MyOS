#include <stdint.h>
#include "irq.h"
#include "pit.h"

static volatile uint32_t ticks = 0;

static void timer_handler(void)
{
    ++ticks;
}

void timer_init(void)
{
    irq_install_handler(0, timer_handler);
    pit_init(100); /* ~100 Hz */
}

uint32_t timer_ticks(void)
{
    return ticks;
}
