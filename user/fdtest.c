/* fdtest: proof program for Phase 14 inc.1 (fd vtable).
 * Exercises BOTH dispatch paths through the per-fd ops in one
 * run — file fds (fcreate/write/close/reopen/read/verify via
 * file_ops) and console fds (stdout writes via console_out_ops)
 * — and asserts the edge semantics the vtable must preserve:
 * write to stdin is -1, read from stdout/stderr is -1, closing
 * a console fd is -1, a closed or never-opened fd is -1, and
 * read with count==0 returns 0 even for an invalid fd (the old
 * sys_read top-level shortcut). Prints one "fdtest: ok" line
 * per check and "fdtest: ALL PASS" at the end; any failure
 * prints "fdtest: FAIL <name>" instead. */
#include "../src/lib/syscall.h"

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts_u(const char *s) { sys_write(1, s, slen(s)); }

static char *itoa(long n, char *buf)
{
    char tmp[24];
    int i = 0, j = 0;
    if (n == 0) { buf[0] = '0'; buf[1] = 0; return buf; }
    int neg = 0;
    if (n < 0) { neg = 1; n = -n; }
    while (n) { tmp[i++] = (char)('0' + (n % 10)); n /= 10; }
    if (neg) tmp[i++] = '-';
    while (i) buf[j++] = tmp[--i];
    buf[j] = 0;
    return buf;
}

static int fails = 0;
static void check(int ok, const char *name)
{
    if (ok) {
        puts_u("fdtest: ok ");
    } else {
        puts_u("fdtest: FAIL ");
        fails++;
    }
    puts_u(name);
    puts_u("\r\n");
}

void _start(long argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[16];

    /* --- file path through file_ops --- */
    long fd = sys_fcreate("fdtest.txt");
    check(fd >= 3, "fcreate returns fd >= 3");
    const char *msg = "vtable-bytes-14";
    check(sys_write(fd, msg, 15) == 15, "file write returns 15");
    check(sys_close(fd) == 0, "file close returns 0");
    check(sys_write(fd, msg, 1) == -1, "write to closed fd is -1");

    fd = sys_open("fdtest.txt");
    check(fd >= 3, "reopen returns fd >= 3");
    char rb[32];
    for (int i = 0; i < 32; i++) rb[i] = 0;
    check(sys_read(fd, rb, 15) == 15, "file read returns 15");
    int same = 1;
    for (int i = 0; i < 15; i++) if (rb[i] != msg[i]) same = 0;
    check(same, "read-back bytes identical");
    check(sys_read(fd, rb, 15) == 0, "read at EOF returns 0");
    check(sys_close(fd) == 0, "second close returns 0");
    check(sys_close(fd) == -1, "double close is -1");

    /* --- console dispatch edges (NULL ops -> -1) --- */
    check(sys_write(0, msg, 1) == -1, "write to stdin is -1");
    check(sys_read(1, rb, 1) == -1, "read from stdout is -1");
    check(sys_read(2, rb, 1) == -1, "read from stderr is -1");
    check(sys_close(0) == -1, "close stdin is -1");
    check(sys_close(1) == -1, "close stdout is -1");
    check(sys_write(9, msg, 1) == -1, "write to free fd is -1");
    check(sys_read(9, rb, 1) == -1, "read from free fd is -1");

    /* --- preserved count==0 asymmetries --- */
    check(sys_read(9, rb, 0) == 0, "read count 0 on invalid fd is 0");
    check(sys_write(1, msg, 0) == 0, "write count 0 on stdout is 0");
    check(sys_write(9, msg, 0) == -1, "write count 0 on free fd is -1");

    /* stdout still works at the end of all that */
    puts_u("fdtest: fails=");
    puts_u(itoa(fails, buf));
    puts_u("\r\n");
    if (!fails) puts_u("fdtest: ALL PASS\r\n");
    sys_exit(fails ? 1 : 0);
}
