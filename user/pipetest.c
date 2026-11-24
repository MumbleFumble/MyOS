/* pipetest: proof program for Phase 14 inc.3 (pipes).
 * Declared semantics under test (non-blocking pipes):
 *   read: buffered bytes first (short counts allowed);
 *         empty with a writer open -> E_WOULDBLOCK (-2);
 *         empty with writers gone -> 0 (EOF, after drain).
 *   write: no readers -> -1; full -> -2; otherwise copies
 *         what fits and returns the short count.
 * Asserts: FIFO order across interleaved chunks, wrap-around
 * past capacity, drain-then-EOF after write-end close,
 * write -1 after read-end close, dup2 aliases of an end
 * behaving as the same end, wrong-end operations failing,
 * pipe() failing cleanly when fewer than two slots are free,
 * and sys_pipe(0) == -1. Every fd opened is closed again.
 * Prints "pipetest: ok <name>" per check and
 * "pipetest: ALL PASS" at the end. */
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
    if (ok) puts_u("pipetest: ok ");
    else { puts_u("pipetest: FAIL "); fails++; }
    puts_u(name);
    puts_u("\r\n");
}

static int bytes_eq(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Deterministic stream pattern: byte at stream position p. */
static unsigned char pat(long p) { return (unsigned char)(p % 251); }

void _start(long argc, char **argv)
{
    (void)argc; (void)argv;
    long fds[2];
    char rb[64];

    check(sys_pipe(0) == -1, "pipe(NULL) is -1");
    check(sys_pipe(fds) == 0, "pipe returns 0");
    long r = fds[0], w = fds[1];
    check(r >= 3 && w > r, "read end in the lower slot");

    /* --- basic FIFO + empty sentinel + count==0 --- */
    check(sys_write(w, "hello", 5) == 5, "write hello returns 5");
    check(sys_read(r, rb, 5) == 5 && bytes_eq(rb, "hello", 5),
          "read returns hello in order");
    check(sys_read(r, rb, 1) == E_WOULDBLOCK,
          "read on empty (writer open) is -2");
    check(sys_write(w, rb, 0) == 0, "write count 0 is 0");
    check(sys_read(r, rb, 0) == 0, "read count 0 is 0");
    check(sys_read(w, rb, 1) == -1, "read from write end is -1");
    check(sys_write(r, "x", 1) == -1, "write to read end is -1");

    /* --- interleaved chunks form one stream --- */
    check(sys_write(w, "ab", 2) == 2, "write ab");
    check(sys_write(w, "cd", 2) == 2, "write cd");
    check(sys_read(r, rb, 3) == 3 && bytes_eq(rb, "abc", 3),
          "read 3 gives abc");
    check(sys_write(w, "ef", 2) == 2, "write ef");
    check(sys_read(r, rb, 3) == 3 && bytes_eq(rb, "def", 3),
          "read 3 gives def");

    /* --- wrap-around: 4000 bytes through a 1024 ring --- */
    {
        unsigned char wb[100], vb[100];
        long wpos = 0, rpos = 0;
        int ok = 1;
        for (int round = 0; round < 40 && ok; round++) {
            for (int i = 0; i < 100; i++) wb[i] = pat(wpos++);
            if (sys_write(w, (char *)wb, 100) != 100) ok = 0;
            if (sys_read(r, (char *)vb, 100) != 100) ok = 0;
            for (int i = 0; i < 100; i++)
                if (vb[i] != pat(rpos++)) ok = 0;
        }
        check(ok && wpos == 4000 && rpos == 4000,
              "wrap-around: 4000 bytes in exact order");
    }

    /* --- full behaviour: fill, -2, partial space, drain --- */
    {
        unsigned char wb[128], vb[128];
        long wpos = 0, rpos = 0;
        int ok = 1;
        for (int chunk = 0; chunk < 8 && ok; chunk++) {
            for (int i = 0; i < 128; i++) wb[i] = pat(wpos++);
            if (sys_write(w, (char *)wb, 128) != 128) ok = 0;
        }
        check(ok && wpos == 1024, "filled exactly 1024 bytes");
        check(sys_write(w, "x", 1) == E_WOULDBLOCK,
              "write on full pipe is -2");
        if (sys_read(r, (char *)vb, 100) != 100) ok = 0;
        for (int i = 0; i < 100; i++) if (vb[i] != pat(rpos++)) ok = 0;
        check(ok, "read 100 from full pipe, pattern holds");
        for (int i = 0; i < 100; i++) wb[i] = pat(wpos++);
        check(sys_write(w, (char *)wb, 100) == 100,
              "write 100 into freed space returns 100");
        check(sys_write(w, "x", 1) == E_WOULDBLOCK,
              "write on refilled pipe is -2 again");
        ok = 1;
        long drained = 0;
        while (drained < 1024 && ok) {
            long n = sys_read(r, (char *)vb, 128);
            if (n <= 0) { ok = 0; break; }
            for (long i = 0; i < n; i++)
                if (vb[i] != pat(rpos++)) ok = 0;
            drained += n;
        }
        check(ok && drained == 1024 && rpos == 1124,
              "drained 1024 bytes in exact order");
        check(sys_read(r, rb, 1) == E_WOULDBLOCK,
              "empty again after drain (writer open)");
    }
    check(sys_close(w) == 0, "close write end");
    check(sys_read(r, rb, 1) == 0, "read after writer close is EOF 0");
    check(sys_close(r) == 0, "close read end");

    /* --- drain-then-EOF with data in flight --- */
    check(sys_pipe(fds) == 0, "pipe 2 created");
    r = fds[0]; w = fds[1];
    check(sys_write(w, "xyz", 3) == 3, "write xyz");
    check(sys_close(w) == 0, "close writer with data buffered");
    check(sys_read(r, rb, 3) == 3 && bytes_eq(rb, "xyz", 3),
          "reader drains buffered bytes");
    check(sys_read(r, rb, 1) == 0, "then EOF 0");
    check(sys_read(r, rb, 1) == 0, "EOF is stable");
    check(sys_close(r) == 0, "close reader 2");

    /* --- write after read-end close is -1 --- */
    check(sys_pipe(fds) == 0, "pipe 3 created");
    r = fds[0]; w = fds[1];
    check(sys_close(r) == 0, "close read end first");
    check(sys_write(w, "a", 1) == -1, "write with no readers is -1");
    check(sys_close(w) == 0, "close write end 3");

    /* --- dup2 alias of the write end is the same end --- */
    check(sys_pipe(fds) == 0, "pipe 4 created");
    r = fds[0]; w = fds[1];
    check(sys_dup2(w, 10) == 10, "dup2 write end to 10");
    check(sys_close(w) == 0, "close original write end");
    check(sys_write(10, "zz", 2) == 2,
          "write via alias still works (pipe not ended)");
    check(sys_read(r, rb, 2) == 2 && bytes_eq(rb, "zz", 2),
          "read the alias bytes");
    check(sys_close(10) == 0, "close last write alias");
    check(sys_read(r, rb, 1) == 0, "EOF once last writer alias gone");
    check(sys_close(r) == 0, "close reader 4");

    /* --- dup2 alias of the read end is the same end --- */
    check(sys_pipe(fds) == 0, "pipe 5 created");
    r = fds[0]; w = fds[1];
    check(sys_dup2(r, 11) == 11, "dup2 read end to 11");
    check(sys_close(r) == 0, "close original read end");
    check(sys_write(w, "q", 1) == 1, "write still has a reader (alias)");
    check(sys_read(11, rb, 1) == 1 && rb[0] == 'q',
          "read via read-end alias");
    check(sys_close(11) == 0, "close read alias");
    check(sys_close(w) == 0, "close writer 5");

    /* --- pipe with fewer than two free slots fails clean --- */
    {
        long files[12];
        int opened = 0;
        for (int i = 0; i < 12; i++) {
            char name[16];
            name[0] = 'p'; name[1] = 'f';
            itoa(i, name + 2);
            files[i] = sys_fcreate(name);
            if (files[i] >= 3) opened++;
        }
        check(opened == 12, "12 files open (one slot left)");
        check(sys_pipe(fds) == -1, "pipe with one free slot is -1");
        long last = sys_fcreate("pfull.txt");
        check(last >= 3, "last slot still free after failed pipe");
        if (last >= 3) sys_close(last);
        int closed = 0;
        for (int i = 0; i < 12; i++) if (sys_close(files[i]) == 0) closed++;
        check(closed == 12, "all 12 files closed");
        check(sys_pipe(fds) == 0, "pipe works again after cleanup");
        check(sys_close(fds[0]) == 0 && sys_close(fds[1]) == 0,
              "cleanup pipe closed");
    }

    puts_u("pipetest: fails=");
    char buf[16];
    puts_u(itoa(fails, buf));
    puts_u("\r\n");
    if (!fails) puts_u("pipetest: ALL PASS\r\n");
    sys_exit(fails ? 1 : 0);
}
