/*
 * hostileprobe.elf - Phase 17 inc.8 probes for the
 * hostile cases no existing fixture covers. Modes:
 *
 *   badnr       issue int 0x80 with out-of-range numbers
 *               (999 and 0); the dispatcher must return
 *               -1 (its ENOSYS default) and the program
 *               survives. Prints "hostileprobe: ok badnr".
 *   recurse     unbounded recursion; the 4-page user
 *               stack overflows into unmapped memory and
 *               the inc-002 kill path must take the task
 *               (EXIT_FAULT). Never returns.
 *   spin        infinite loop; never exits. Containment
 *               is preemption, proven by run-spin.
 *   run-recurse spawn "recurse", collect EXIT_FAULT via
 *               SYS_WAIT, print "hostileprobe: ok recurse".
 *   run-spin    spawn "spin" WITHOUT waiting, then spawn
 *               argtest.elf and collect its 0 exit while
 *               the spinner is still running - proof the
 *               spinner does not starve a second program.
 *               The spinner is deliberately left running
 *               (there is no kill syscall); the test's
 *               QEMU teardown ends it. Prints
 *               "hostileprobe: ok spin".
 *   int3        raise #BP from ring 3. CHARACTERISATION
 *               ONLY - vector 3 has no IDT gate (a known
 *               residual), so this is expected to reset
 *               the machine. Never invoked by the suite
 *               test; used once, by hand, to characterise
 *               the residual honestly.
 */
#include "../src/lib/syscall.h"

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sys_write(1, s, slen(s)); }
static int streq(const char *a, const char *b)
{
    long i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

static long raw_nr(long nr)
{
    long ret;
    asm volatile("int $0x80" : "=a"(ret)
                 : "a"(nr), "D"(0L), "S"(0L), "d"(0L)
                 : "rcx", "r11", "memory");
    return ret;
}

static volatile char sink;

/* The frame is written through a volatile pointer and
 * read again after the recursive call into a volatile
 * store, and the function is noinline: every level must
 * materialise a real stack frame that stays live across
 * the call, so the descent cannot be collapsed into a
 * loop (a first version without these defences was -
 * the child spun forever and never faulted). */
__attribute__((noinline))
static long rec(long d)
{
    char frame[128];
    volatile char *f = frame;
    f[0] = (char)d;
    f[127] = (char)(d >> 4);
    long r = rec(d + 1);
    sink = (char)(f[0] ^ f[127] ^ (char)r);
    return r + 1;
}

void _start(long argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "";
    if (streq(mode, "badnr")) {
        long a = raw_nr(999);
        long b = raw_nr(0);
        if (a == -1 && b == -1) {
            puts1("hostileprobe: ok badnr\r\n");
            sys_exit(0);
        }
        puts1("hostileprobe: FAIL badnr\r\n");
        sys_exit(1);
    }
    if (streq(mode, "recurse")) {
        sink = (char)rec(0);
        puts1("hostileprobe: FAIL recurse survived\r\n");
        sys_exit(1);
    }
    if (streq(mode, "spin")) {
        for (;;) sink++;
    }
    if (streq(mode, "run-recurse")) {
        long pid = sys_exec("hostileprobe.elf", "hostileprobe.elf recurse");
        long code = (pid > 0) ? sys_wait(pid) : -1;
        if (code == EXIT_FAULT) {
            puts1("hostileprobe: ok recurse\r\n");
            sys_exit(0);
        }
        puts1("hostileprobe: FAIL recurse\r\n");
        sys_exit(1);
    }
    if (streq(mode, "run-spin")) {
        long spin = sys_exec("hostileprobe.elf", "hostileprobe.elf spin");
        long pid = sys_exec("argtest.elf", "argtest.elf one two");
        long code = (pid > 0) ? sys_wait(pid) : -1;
        if (spin > 0 && code == 0) {
            puts1("hostileprobe: ok spin\r\n");
            sys_exit(0);
        }
        puts1("hostileprobe: FAIL spin\r\n");
        sys_exit(1);
    }
    if (streq(mode, "int3")) {
        asm volatile("int3");
        puts1("hostileprobe: int3 survived\r\n");
        sys_exit(0);
    }
    puts1("hostileprobe: FAIL unknown mode\r\n");
    sys_exit(1);
}
