/*
 * fdprobe.elf - Phase 17 inc.5 default-table probe.
 *
 * Spawned WITHOUT inheritance: verifies fds 0/1/2 are the
 * console descriptions exactly as before this increment -
 * fd 0 refuses writes, fd 1 refuses reads, and the console
 * slots are still pinned (close refuses). Prints its
 * verdict on fd 1 and exits 0 on success, 1 otherwise.
 */
#include "../src/lib/syscall.h"

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sys_write(1, s, slen(s)); }

void _start(long argc, char **argv)
{
    (void)argc; (void)argv;
    char b[4];
    int ok = 1;
    if (sys_write(0, "x", 1) != -1) ok = 0;   /* console in: no write op */
    if (sys_read(1, b, 1) != -1) ok = 0;      /* console out: no read op */
    if (sys_read(2, b, 1) != -1) ok = 0;
    if (sys_close(0) != -1) ok = 0;           /* pinned */
    if (sys_close(1) != -1) ok = 0;           /* pinned */
    puts1(ok ? "fdprobe: console OK\r\n" : "fdprobe: console FAIL\r\n");
    sys_exit(ok ? 0 : 1);
}
