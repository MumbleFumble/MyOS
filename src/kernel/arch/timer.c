#include <stdint.h>
#include "irq.h"
#include "pit.h"
#include "../proc/sched.h"

static volatile uint32_t ticks = 0;

static void timer_handler(void)
{
    ++ticks;
    sched_tick((uint64_t)ticks);
}

/* -----------------------------------------------------------------------
 * TSC calibration (Phase 3, increment 2).
 *
 * The PIT runs at 100 Hz (10 ms per tick) -- far too coarse for I/O
 * latency measurement. The x86 timestamp counter (rdtsc) gives
 * sub-microsecond resolution. We calibrate TSC frequency once at boot
 * by measuring TSC ticks across 10 PIT ticks (~100 ms).
 * ----------------------------------------------------------------------- */
static uint64_t tsc_freq_hz = 0;

static inline uint64_t rdtsc_raw(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t timer_tsc(void)
{
    return rdtsc_raw();
}

uint64_t timer_tsc_freq(void)
{
    return tsc_freq_hz;
}

static void tsc_calibrate(void)
{
    /* Wait for a PIT tick boundary, then measure over 10 ticks. */
    uint32_t t0 = ticks;
    while (ticks == t0) { /* spin */ }
    uint64_t c0 = rdtsc_raw();
    uint32_t target = ticks + 10;
    while (ticks < target) { /* spin */ }
    uint64_t c1 = rdtsc_raw();
    /* 10 ticks at 100 Hz = 0.1 s */
    tsc_freq_hz = (c1 - c0) * 10;
}

void timer_init(void)
{
    irq_install_handler(0, timer_handler);
    pit_init(100); /* ~100 Hz */
    tsc_calibrate();
}

uint32_t timer_ticks(void)
{
    return ticks;
}
