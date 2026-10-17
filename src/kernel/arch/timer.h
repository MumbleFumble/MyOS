#pragma once

#include <stdint.h>

void timer_init(void);
uint32_t timer_ticks(void);

/* TSC (timestamp counter) interface -- fine-grained timing for I/O telemetry.
 * Calibrated against the PIT at boot. Returns raw TSC ticks; divide by
 * timer_tsc_freq() for seconds. */
uint64_t timer_tsc(void);
uint64_t timer_tsc_freq(void);
