#include "../src/lib/syscall.h"

/*
 * bench_files — interleaved two-file read benchmark (Phase 3 inc.10).
 *
 * Opens io_test.bin and io_test2.bin (1 MiB each) and reads them
 * alternately in 4 KiB chunks (8 sectors per chunk) until both are
 * exhausted, for ROUNDS rounds: the two-stream pattern that defeats
 * global LBA heuristics. Each file's own stream is purely sequential;
 * only per-file state can see that. Checksums both files and reports
 * scheduler telemetry like bench_io.
 *
 * Run from the shell with:
 *     exec bench_files.elf
 */

#define FILE_A "io_test.bin"
#define FILE_B "io_test2.bin"
#define ROUNDS 3L
#define CHUNK  4096L

static char io_buf[4096];

static long slen(const char *s) {
    long n = 0; while (s[n]) n++; return n;
}
static void puts_u(const char *s) { sys_write(STDOUT, s, slen(s)); }

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

    puts_u("bench_files: interleaved read, rounds=");
    puts_u(itoa(ROUNDS, buf));
    puts_u(" files=" FILE_A "+" FILE_B "\r\n");
    if (have_before) {
        puts_u("bench_files: start ticks=");
        puts_u(itoa((long)before.total_ticks, buf));
        puts_u(" syscalls=");
        puts_u(itoa((long)before.syscall_count, buf));
        puts_u("\r\n");
    }

    unsigned long total_bytes = 0;
    unsigned long checksum = 0;
    for (long round = 0; round < ROUNDS; round++) {
        long fa = sys_open(FILE_A);
        long fb = sys_open(FILE_B);
        if (fa < 0 || fb < 0) {
            puts_u("bench_files: open failed round=");
            puts_u(itoa(round, buf));
            puts_u("\r\n"); sys_exit(1);
        }
        int done_a = 0, done_b = 0;
        while (!done_a || !done_b) {
            if (!done_a) {
                long r = sys_read(fa, io_buf, CHUNK);
                if (r < 0) { puts_u("bench_files: read A failed\r\n"); sys_exit(1); }
                if (r == 0) done_a = 1;
                else {
                    total_bytes += (unsigned long)r;
                    for (long i = 0; i < r; i++)
                        checksum = checksum * 31 + (unsigned char)io_buf[i];
                }
            }
            if (!done_b) {
                long r = sys_read(fb, io_buf, CHUNK);
                if (r < 0) { puts_u("bench_files: read B failed\r\n"); sys_exit(1); }
                if (r == 0) done_b = 1;
                else {
                    total_bytes += (unsigned long)r;
                    for (long i = 0; i < r; i++)
                        checksum = checksum * 31 + (unsigned char)io_buf[i];
                }
            }
        }
        sys_close(fa);
        sys_close(fb);
    }

    if (self_stats(&after) == 0) {
        puts_u("bench_files: end   ticks=");
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

    puts_u("bench_files: bytes=");
    puts_u(itoa((long)total_bytes, buf));
    puts_u(" checksum=");
    puts_u(itoa((long)checksum, buf));
    puts_u(" done\r\n");
    sys_exit(0);
}
