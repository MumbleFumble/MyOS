/* badptr — hostile-pointer probe (Phase 17 inc.3). Passes
 * bad user pointers to the core syscalls and checks that
 * each is refused with EFAULT (-3) while the program keeps
 * running; then proves a valid round-trip still works
 * byte-exact and that a kernel-visible value (its own pid)
 * is unchanged after an attempted write through a
 * kernel-range pointer — the write that, before the
 * validated copy layer, would have corrupted kernel
 * memory. The probe cannot read kernel memory back (that
 * is the point), so pid stability plus the suite staying
 * green is the corruption evidence, not a direct readback.
 */
#include "../src/lib/syscall.h"

#define DEMAND_BASE 0x20000000000UL

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts_u(const char *s) { sys_write(1, s, slen(s)); }
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

static int fails;
static void expect(const char *name, long got, long want)
{
    if (got == want) {
        puts_u("badptr: ok ");
        puts_u(name);
        puts_u("\r\n");
    } else {
        puts_u("badptr: FAIL ");
        puts_u(name);
        puts_u(" got ");
        putnum(got);
        puts_u(" want ");
        putnum(want);
        puts_u("\r\n");
        fails++;
    }
}

void _start(long argc, char **argv)
{
    (void)argc; (void)argv;
    long pid0 = sys_getpid();
    puts_u("badptr: start pid ");
    putnum(pid0);
    puts_u("\r\n");

    static const char payload[] = "badptr-roundtrip-17";
    long fd = sys_fcreate("badptr.txt");
    if (fd < 0) { puts_u("badptr: FAIL fcreate\r\n"); sys_exit(1); }
    if (sys_write(fd, payload, 19) != 19) {
        puts_u("badptr: FAIL valid write\r\n");
        sys_exit(1);
    }

    /* Bad buffers to sys_write / sys_read on a real fd. */
    expect("write-kernel", sys_write(fd, (const void *)0x1000, 4),
           EFAULT);
    expect("write-unmapped",
           sys_write(fd, (const void *)0x50000000000UL, 4), EFAULT);
    /* Straddle: touch demand page 0 from ring 3, then pass
     * a range spanning it and the untouched page 1. */
    *(volatile char *)DEMAND_BASE = 7;
    expect("write-straddle",
           sys_write(fd, (const void *)(DEMAND_BASE + 4090), 16),
           EFAULT);
    sys_close(fd);

    fd = sys_open("badptr.txt");
    if (fd < 0) { puts_u("badptr: FAIL reopen\r\n"); sys_exit(1); }
    char buf[32];
    expect("read-kernel", sys_read(fd, (void *)0x1000, 4), EFAULT);
    expect("read-unmapped",
           sys_read(fd, (void *)0x50000000000UL, 4), EFAULT);

    /* A bad buffer on the console fd is refused too, and
     * this program keeps printing afterwards. */
    expect("write-console-kernel",
           sys_write(1, (const void *)0x1000, 4), EFAULT);

    /* Bad path pointers to sys_open. */
    expect("open-kernel", sys_open((const char *)0x1000), EFAULT);
    expect("open-unmapped",
           sys_open((const char *)0x50000000000UL), EFAULT);

    /* sys_pipe: bad array refused; NULL keeps its pinned -1. */
    expect("pipe-kernel", sys_pipe((long *)0x1000), EFAULT);
    expect("pipe-null", sys_pipe(0), -1);

    /* Kernel-visible value unchanged by the attempts. */
    if (sys_getpid() == pid0) puts_u("badptr: pid stable\r\n");
    else { puts_u("badptr: FAIL pid changed\r\n"); fails++; }

    /* Valid round-trip, byte-exact. */
    long n = sys_read(fd, buf, 19);
    int same = (n == 19);
    for (int i = 0; same && i < 19; i++)
        if (buf[i] != payload[i]) same = 0;
    if (same) puts_u("badptr: roundtrip OK\r\n");
    else { puts_u("badptr: FAIL roundtrip\r\n"); fails++; }
    sys_close(fd);

    if (!fails) puts_u("badptr: ALL PASS\r\n");
    else puts_u("badptr: FAILURES\r\n");
    sys_exit(fails ? 1 : 0);
}
