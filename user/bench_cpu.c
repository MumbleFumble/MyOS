#include "../src/lib/syscall.h"

/*
 * bench_cpu — CPU-bound benchmark workload.
 *
 * Runs ITERS iterations of a deterministic integer-mix loop (data-dependent
 * chain, volatile accumulator so the compiler cannot eliminate it), then
 * reports its own scheduler telemetry (ticks, syscall count) via
 * SYS_TELEMETRY plus a checksum of the computation so runs are verifiable
 * and comparable across policies.
 *
 * Build: wired into the Makefile next to hello.elf; the ELF is packed into
 * disk.img by the `disk` target. Run from the shell with:
 *     exec bench_cpu.elf
 */

#define ITERS 20000000UL

static long slen(const char *s) {
    long n = 0; while (s[n]) n++; return n;
}
static void puts_u(const char *s) { sys_write(STDOUT, s, slen(s)); }

/* Minimal itoa: writes decimal representation of n into buf, returns pointer */
static char *itoa(long n, char *buf)
{
    char tmp[24]; int i = 0;
    if (n == 0) { buf[0]='0'; buf[1]='\0'; return buf; }
    int neg = n < 0; if (neg) n = -n;
    while (n) { tmp[i++] = '0' + (n % 10); n /= 10; }
    if (neg) tmp[i++] = '-';
    int j = 0;
    while (i--) buf[j++] = tmp[i];
    buf[j] = '\0';
    return buf;
}

/* Deterministic integer-mix work kernel. The volatile accumulator and the
 * data-dependent chain defeat dead-code elimination and auto-vectorization
 * differences; the output checksum is stable across runs. */
static unsigned long work_kernel(unsigned long iters)
{
    volatile unsigned long acc = 0x12345678UL;
    for (unsigned long i = 0; i < iters; i++) {
        unsigned long x = acc ^ (i + 0x9E3779B97F4A7C15UL);
        x *= 6364136223846793005UL;
        x ^= x >> 29;
        x += 1442695040888963407UL;
        acc = x;
    }
    return acc;
}

/* Fill *out with this task's telemetry snapshot. Returns 0 on success. */
static int self_stats(task_stat_t *out)
{
    static task_stat_t stats[32];
    long pid = sys_getpid();
    long n = sys_telemetry(stats, 32);
    if (n < 0) return -1;
    for (long i = 0; i < n; i++) {
        if ((long)stats[i].pid == pid) { *out = stats[i]; return 0; }
    }
    return -1;
}

void _start(long argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[32];
    task_stat_t before, after;
    int have_before = self_stats(&before) == 0;

    puts_u("bench_cpu: running ");
    puts_u(itoa((long)ITERS, buf));
    puts_u(" iterations\r\n");
    if (have_before) {
        puts_u("bench_cpu: start ticks=");
        puts_u(itoa((long)before.total_ticks, buf));
        puts_u(" syscalls=");
        puts_u(itoa((long)before.syscall_count, buf));
        puts_u("\r\n");
    }

    unsigned long acc = work_kernel(ITERS);

    if (self_stats(&after) == 0) {
        puts_u("bench_cpu: end   ticks=");
        puts_u(itoa((long)after.total_ticks, buf));
        if (have_before) {
            puts_u(" (+");
            puts_u(itoa((long)(after.total_ticks - before.total_ticks), buf));
            puts_u(")");
        }
        puts_u(" syscalls=");
        puts_u(itoa((long)after.syscall_count, buf));
        puts_u("\r\n");
    }

    puts_u("bench_cpu: checksum=");
    puts_u(itoa((long)acc, buf));
    puts_u(" done\r\n");
    sys_exit(0);
}
