/* faulttest — waiter harness for the fault path
 * (Phase 17 inc.2). Usage: faulttest <mode>
 * Spawns faultprobe.elf <mode> via SYS_EXEC, collects its
 * exit code via SYS_WAIT, and checks it: EXIT_FAULT for
 * every faulting mode, 0 for "ok". Prints one verdict line
 * per run and exits 0 only when the code matched, so this
 * program is also the first test in the tree that collects
 * a nonzero exit code through SYS_WAIT at all.
 */
#include "../src/lib/syscall.h"

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts_u(const char *s) { sys_write(1, s, slen(s)); }
static int streq(const char *a, const char *b)
{
    long i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}
static void putnum(long v)
{
    char tmp[24], out[24];
    int i = 0, j = 0;
    if (v == 0) { puts_u("0"); return; }
    if (v < 0) { puts_u("-"); v = -v; }
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) out[j++] = tmp[--i];
    out[j] = 0;
    puts_u(out);
}

void _start(long argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "null";
    long expected = streq(mode, "ok") ? 0 : EXIT_FAULT;

    char cmdline[64];
    long ci = 0;
    const char *base = "faultprobe.elf ";
    for (long i = 0; base[i]; i++) cmdline[ci++] = base[i];
    for (long i = 0; mode[i] && ci < 62; i++) cmdline[ci++] = mode[i];
    cmdline[ci] = 0;

    long pid = sys_exec("faultprobe.elf", cmdline);
    if (pid < 0) {
        puts_u("faulttest: FAIL exec\r\n");
        sys_exit(1);
    }
    long code = sys_wait(pid);
    puts_u("faulttest: ");
    puts_u(mode);
    puts_u(" exit=");
    putnum(code);
    puts_u("\r\n");
    if (code == expected) {
        puts_u("faulttest: PASS ");
        puts_u(mode);
        puts_u("\r\n");
        sys_exit(0);
    }
    puts_u("faulttest: FAIL ");
    puts_u(mode);
    puts_u("\r\n");
    sys_exit(1);
}
