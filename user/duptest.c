/* duptest: proof program for Phase 14 inc.2 (dup2).
 * Asserts the shared-description semantics, not just the
 * happy path:
 *  - interleaved reads through two aliases observe ONE
 *    offset ("012" via the original, "345" via the alias);
 *  - interleaved writes append as one stream ("abcd");
 *  - closing one alias leaves the other fully usable;
 *    closing the last alias closes the backend exactly once
 *    (both numbers dead afterwards, file itself intact);
 *  - dup2 onto an occupied fd displaces it correctly;
 *  - invalid arguments return -1 (free oldfd, dup2(9,9),
 *    out-of-range newfd); dup2(fd, fd) is a no-op success;
 *  - redirection shape: save stdout with dup2(1, 9), dup2 a
 *    file over fd 1, write via fd 1 (bytes land in the file),
 *    close(1) succeeds on the unpinned slot, restore fd 1
 *    from the saved alias, close the alias;
 *  - a console alias (dup2(1, 6)) writes to the console and
 *    closes, while a never-displaced console fd (fd 2) stays
 *    pinned: close(2) == -1;
 *  - the fd table fills at 13 file fds and the 14th fails.
 * Prints "duptest: ok <name>" per check and
 * "duptest: ALL PASS" at the end. */
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
    if (ok) puts_u("duptest: ok ");
    else { puts_u("duptest: FAIL "); fails++; }
    puts_u(name);
    puts_u("\r\n");
}

static int bytes_eq(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

void _start(long argc, char **argv)
{
    (void)argc; (void)argv;
    char rb[32];

    /* --- shared offset via interleaved reads --- */
    long fd = sys_fcreate("dup2.txt");
    check(fd >= 3, "fcreate fd >= 3");
    check(sys_write(fd, "0123456789", 10) == 10, "write 10 bytes");
    check(sys_close(fd) == 0, "close after write");
    fd = sys_open("dup2.txt");
    check(fd >= 3, "reopen fd >= 3");
    check(sys_dup2(fd, 7) == 7, "dup2(fd, 7) returns 7");
    check(sys_read(fd, rb, 3) == 3 && bytes_eq(rb, "012", 3),
          "read via original gets 012");
    check(sys_read(7, rb, 3) == 3 && bytes_eq(rb, "345", 3),
          "read via alias continues at shared offset (345)");

    /* --- close one alias, the other stays usable --- */
    check(sys_close(fd) == 0, "close original alias");
    check(sys_read(7, rb, 4) == 4 && bytes_eq(rb, "6789", 4),
          "alias still reads after original closed");
    check(sys_read(7, rb, 1) == 0, "alias read at EOF is 0");
    check(sys_close(7) == 0, "close last alias");
    check(sys_read(7, rb, 1) == -1, "fd dead after last close");
    check(sys_close(7) == -1, "second close of dead fd is -1");
    fd = sys_open("dup2.txt");
    check(sys_read(fd, rb, 10) == 10 && bytes_eq(rb, "0123456789", 10),
          "file intact after backend closed exactly once");
    check(sys_close(fd) == 0, "close verify fd");

    /* --- shared offset via interleaved writes --- */
    fd = sys_fcreate("dupw.txt");
    check(sys_dup2(fd, 8) == 8, "dup2(fd, 8) returns 8");
    check(sys_write(fd, "ab", 2) == 2, "write ab via original");
    check(sys_write(8, "cd", 2) == 2, "write cd via alias");
    check(sys_close(fd) == 0, "close write original");
    check(sys_close(8) == 0, "close write alias");
    fd = sys_open("dupw.txt");
    check(sys_read(fd, rb, 4) == 4 && bytes_eq(rb, "abcd", 4),
          "interleaved writes form one stream abcd");
    check(sys_close(fd) == 0, "close abcd verify fd");

    /* --- dup2 onto an occupied fd displaces it --- */
    long fa = sys_fcreate("dupA.txt");
    check(sys_write(fa, "AAAA", 4) == 4, "write AAAA");
    long fb = sys_fcreate("dupB.txt");
    check(sys_write(fb, "BBBB", 4) == 4, "write BBBB");
    check(sys_close(fa) == 0 && sys_close(fb) == 0, "close A and B writers");
    fa = sys_open("dupA.txt");
    fb = sys_open("dupB.txt");
    check(fa >= 3 && fb >= 3 && fa != fb, "reopen A and B");
    check(sys_dup2(fa, fb) == fb, "dup2(A, B) returns B fd");
    check(sys_read(fb, rb, 4) == 4 && bytes_eq(rb, "AAAA", 4),
          "displaced fd now reads A");
    check(sys_close(fa) == 0, "close A original");
    check(sys_close(fb) == 0, "close A alias (backend closes once)");
    fb = sys_open("dupB.txt");
    check(sys_read(fb, rb, 4) == 4 && bytes_eq(rb, "BBBB", 4),
          "displaced file B intact");
    check(sys_close(fb) == 0, "close B verify fd");

    /* --- invalid arguments and the no-op case --- */
    check(sys_dup2(9, 5) == -1, "dup2 from free fd is -1");
    check(sys_dup2(9, 9) == -1, "dup2(9, 9) on free fd is -1");
    fd = sys_open("dup2.txt");
    check(sys_dup2(fd, 16) == -1, "dup2 to out-of-range fd is -1");
    check(sys_dup2(fd, fd) == fd, "dup2(fd, fd) is a no-op success");
    check(sys_read(fd, rb, 2) == 2 && bytes_eq(rb, "01", 2),
          "fd still usable after no-op dup2");
    check(sys_close(fd) == 0, "close no-op fd");

    /* --- redirection shape: save, displace fd 1, restore ---
     * No check() calls while fd 1 points at the file: the
     * results are verified after stdout is restored. */
    fd = sys_fcreate("dupr.txt");
    long w0 = sys_write(fd, "redirected", 10);
    long saved = sys_dup2(1, 9);
    long d1 = sys_dup2(fd, 1);
    long wr = sys_write(1, "via-redirect", 12);
    long c1 = sys_close(1);
    long rs = sys_dup2(saved, 1);
    long cs = sys_close(saved);
    long cf = sys_close(fd);
    check(w0 == 10, "write redirected file via its fd");
    check(saved == 9, "dup2(1, 9) saves stdout");
    check(d1 == 1, "dup2(file, 1) returns 1");
    check(wr == 12, "write via displaced fd 1 returns 12");
    check(c1 == 0, "close(1) succeeds once unpinned by dup2");
    check(rs == 1, "dup2(saved, 1) restores stdout");
    check(cs == 0, "close saved console alias succeeds");
    check(cf == 0, "close redirect original (backend once)");
    fd = sys_open("dupr.txt");
    check(sys_read(fd, rb, 22) == 22 &&
          bytes_eq(rb, "redirectedvia-redirect", 22),
          "bytes written via fd 1 landed in the file");
    check(sys_close(fd) == 0, "close redirect verify fd");

    /* --- console aliasing --- */
    check(sys_dup2(1, 6) == 6, "dup2(1, 6) console alias returns 6");
    check(sys_write(6, "duptest: console via alias\r\n", 28) == 28,
          "write via console alias returns 28");
    check(sys_close(6) == 0, "close console alias succeeds");
    check(sys_close(2) == -1, "close(2) still pinned -1");

    /* --- table fills: 13 file fds, the 14th fails --- */
    long fds[13];
    int opened = 0;
    for (int i = 0; i < 13; i++) {
        char name[16];
        name[0] = 'd'; name[1] = 'u'; name[2] = 'p'; name[3] = 'f';
        itoa(i, name + 4);
        fds[i] = sys_fcreate(name);
        if (fds[i] >= 3) opened++;
    }
    check(opened == 13, "13 file fds open at once");
    check(sys_fcreate("dupfull.txt") == -1, "14th open fails (table full)");
    int closed = 0;
    for (int i = 0; i < 13; i++) if (sys_close(fds[i]) == 0) closed++;
    check(closed == 13, "all 13 close cleanly");

    puts_u("duptest: fails=");
    char buf[16];
    puts_u(itoa(fails, buf));
    puts_u("\r\n");
    if (!fails) puts_u("duptest: ALL PASS\r\n");
    sys_exit(fails ? 1 : 0);
}
