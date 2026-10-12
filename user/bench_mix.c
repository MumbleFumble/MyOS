#include "../src/lib/syscall.h"

/*
 * bench_mix — mixed CPU+I/O benchmark workload.
 *
 * Interleaves CPU bursts with I/O bursts over ROUNDS rounds. Each round:
 *   1. CPU burst: CPU_ITERS iterations of a deterministic integer-mix loop
 *      (same kernel as bench_cpu: data-dependent chain, volatile
 *      accumulator).
 *   2. I/O burst: reads the 1 MiB io_test.bin file fully in 4 KiB chunks
 *      via sys_open / sys_read / sys_close.
 *
 * The phase changes give the workload a different scheduler signature from
 * the pure benchmarks: moderate syscall rate with alternating compute- and
 * device-wait-heavy stretches. Reports its own scheduler telemetry
 * (ticks, syscall count, I/O-block count) via SYS_TELEMETRY, plus bytes
 * read and a combined checksum so runs are verifiable and comparable.
 *
 * NOTE: like bench_io, io_block_count stays 0 — the ATA driver uses
 * polling PIO, so disk reads never put the task to sleep.
 *
 * Build: wired into the Makefile next to the other benchmarks; the ELF is
 * packed into disk.img by the `disk` target (io_test.bin is already there).
 * Run from the shell with:
 *     exec bench_mix.elf
 */

#define IO_TEST_FILE "io_test.bin"
#define ROUNDS       10L
#define CPU_ITERS    1000000UL
#define CHUNK        4096L

static char io_buf[4096];

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

/* Deterministic CPU-bound work kernel (same as bench_cpu). */
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

    puts_u("bench_mix: mixed workload, rounds=");
    puts_u(itoa(ROUNDS, buf));
    puts_u(" cpu_iters/round=");
    puts_u(itoa((long)CPU_ITERS, buf));
    puts_u(" file=" IO_TEST_FILE "\r\n");
    if (have_before) {
        puts_u("bench_mix: start ticks=");
        puts_u(itoa((long)before.total_ticks, buf));
        puts_u(" syscalls=");
        puts_u(itoa((long)before.syscall_count, buf));
        puts_u(" io_blocks=");
        puts_u(itoa((long)before.io_block_count, buf));
        puts_u("\r\n");
    }

    unsigned long total_bytes = 0;
    unsigned long checksum = 0;
    for (long p = 0; p < ROUNDS; p++) {
        /* CPU burst */
        unsigned long acc = work_kernel(CPU_ITERS);
        checksum = checksum * 31 + acc;

        /* I/O burst: full sequential read of the test file */
        long fd = sys_open(IO_TEST_FILE);
        if (fd < 0) {
            puts_u("bench_mix: open failed round=");
            puts_u(itoa(p, buf));
            puts_u("\r\n"); sys_exit(1);
        }
        long r;
        while ((r = sys_read(fd, io_buf, CHUNK)) > 0) {
            total_bytes += (unsigned long)r;
            for (long i = 0; i < r; i++)
                checksum = checksum * 31 + (unsigned char)io_buf[i];
        }
        if (r < 0) {
            puts_u("bench_mix: read failed round=");
            puts_u(itoa(p, buf));
            puts_u(" bytes_so_far=");
            puts_u(itoa((long)total_bytes, buf));
            puts_u("\r\n"); sys_exit(1);
        }
        sys_close(fd);
    }

    if (self_stats(&after) == 0) {
        puts_u("bench_mix: end   ticks=");
        puts_u(itoa((long)after.total_ticks, buf));
        if (have_before) {
            puts_u(" (+");
            puts_u(itoa((long)(after.total_ticks - before.total_ticks), buf));
            puts_u(")");
        }
        puts_u(" syscalls=");
        puts_u(itoa((long)after.syscall_count, buf));
        puts_u(" (+");
        puts_u(itoa((long)(after.syscall_count - before.syscall_count), buf));
        puts_u(") io_blocks=");
        puts_u(itoa((long)after.io_block_count, buf));
        puts_u("\r\n");
    }

    puts_u("bench_mix: bytes=");
    puts_u(itoa((long)total_bytes, buf));
    puts_u(" checksum=");
    puts_u(itoa((long)checksum, buf));
    puts_u(" done\r\n");
    sys_exit(0);
}
