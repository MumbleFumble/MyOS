#include "../src/lib/syscall.h"

/*
 * evict_test -- Phase 2 eviction test (increment 4).
 *
 * The demand region (0x20000000000, 16 pages) has a 4-frame quota with a
 * FIFO eviction policy. This test touches all 16 pages sequentially:
 *   - pages 0..3:  4 faults, fill the quota (no evictions)
 *   - pages 4..15: 12 faults, each evicts the oldest resident page
 * Then it re-touches page 0 (evicted long ago): 1 more fault + 1 eviction.
 *
 * Expected kernel serial output: 17 "[pf] demand-mapped" lines and
 * 13 "[pf] evicting" lines, no halt. Prints "[evict] PASS".
 *
 * Build: wired into the Makefile next to the benchmarks; the ELF is
 * packed into disk.img by the `disk` target. Run from the shell with:
 *     exec evict_test.elf
 */

#define DEMAND_BASE 0x20000000000UL
#define NPAGES      16

static long slen(const char *s) {
    long n = 0; while (s[n]) n++; return n;
}
static void puts_u(const char *s) { sys_write(STDOUT, s, slen(s)); }

void _start(long argc, char **argv)
{
    (void)argc; (void)argv;
    volatile unsigned char *base = (volatile unsigned char *)DEMAND_BASE;

    puts_u("evict_test: touching 16 pages with 4-frame quota\r\n");

    /* Touch all 16 pages. Each first touch faults; pages 4..15 evict. */
    for (long p = 0; p < NPAGES; p++) {
        volatile unsigned char *pg = base + p * 4096;
        pg[0] = (unsigned char)(0xA0 + p);
    }

    puts_u("evict_test: re-touching page 0 (should fault again)\r\n");
    volatile unsigned char v = base[0];
    (void)v;

    puts_u("[evict] PASS: 16 pages touched, page 0 re-touched\r\n");
    sys_exit(0);
}
