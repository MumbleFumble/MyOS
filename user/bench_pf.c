#include "../src/lib/syscall.h"

/*
 * bench_pf -- Phase 2 page-fault benchmark (increment 5).
 *
 * Deterministic multi-phase access pattern over the 16-page demand region
 * (0x20000000000), designed to discriminate eviction policies:
 *
 *   Phase A: sequential scan of pages 0..7, twice (16 accesses).
 *            Punishes LRU/FIFO (0% hits on scans > cache); rewards
 *            furthest-next-use eviction.
 *   Phase B: hot loop over pages 0..5, 20 iterations (120 accesses).
 *            Working set (6) > quota (4): tests thrashing behavior.
 *   Phase C: 100 accesses, 80% hot pages (0..3), 20% cold (8..15),
 *            deterministic pseudo-random via LCG. Tests hot/cold
 *            discrimination.
 *
 * Total: 236 accesses. Each access writes one byte (forces the fault
 * if the page is not resident). Prints "[bench_pf] DONE" when finished.
 *
 * The same pattern is generated in Python by scripts/sim_pf_search.py
 * for offline policy search.
 */

#define DEMAND_BASE 0x20000000000UL

static long slen(const char *s) {
    long n = 0; while (s[n]) n++; return n;
}
static void puts_u(const char *s) { sys_write(STDOUT, s, slen(s)); }

/* Deterministic LCG matching the Python simulator. */
static unsigned long lcg_state = 12345;
static unsigned long lcg_next(void) {
    lcg_state = lcg_state * 1103515245UL + 12345UL;
    return (lcg_state >> 16) & 0x7FFFUL;
}

static void touch(int page) {
    volatile unsigned char *p =
        (volatile unsigned char *)(DEMAND_BASE + (unsigned long)page * 4096UL);
    *p = (unsigned char)page;
}

void _start(long argc, char **argv)
{
    (void)argc; (void)argv;

    /* Phase A: sequential scan 0..7 twice. */
    for (int r = 0; r < 2; r++)
        for (int p = 0; p < 8; p++)
            touch(p);

    /* Phase B: hot loop over 0..5, 20 iterations. */
    for (int r = 0; r < 20; r++)
        for (int p = 0; p < 6; p++)
            touch(p);

    /* Phase C: 80/20 hot/cold, 100 accesses via LCG. */
    for (int i = 0; i < 100; i++) {
        unsigned long v = lcg_next() % 100UL;
        int page;
        if (v < 80)
            page = (int)(lcg_next() % 4UL);          /* hot: 0..3 */
        else
            page = 8 + (int)(lcg_next() % 8UL);      /* cold: 8..15 */
        touch(page);
    }

    puts_u("[bench_pf] DONE: 236 accesses\r\n");
    sys_exit(0);
}
