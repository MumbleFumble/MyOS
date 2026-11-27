/*
 * tooltest.elf - Phase 17 inc.6 harness for cat.elf and
 * wc.elf, wired through SYS_EXEC_INHERIT (inc.005).
 *
 * Runs:
 *  (1) cat file mode -> pipe -> wc: three payloads
 *      (empty, no trailing newline, 2000 bytes crossing
 *      the 1024-byte ring), asserting wc's exact output
 *      line and both children's exit codes;
 *  (2) cat stdin mode round trip through two pipes with
 *      the 2000-byte payload, byte-exact, EOF observed;
 *  (3) cat on a nonexistent file exits with its stated
 *      open-failure code (2), collected via SYS_WAIT.
 * Every read loop is bounded; EOF must arrive via the
 * children's closes (their exit tears down nothing).
 */
#include "../src/lib/syscall.h"

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sys_write(1, s, slen(s)); }

static int fails = 0;
static void check(int cond, const char *name)
{
    if (cond) { puts1("tooltest: ok "); puts1(name); puts1("\r\n"); }
    else { puts1("tooltest: FAIL "); puts1(name); puts1("\r\n"); fails++; }
}

static int write_all(long fd, const char *data, long len)
{
    long off = 0;
    long spins = 0;
    while (off < len) {
        long w = sys_write(fd, data + off, len - off);
        if (w == E_WOULDBLOCK) { if (++spins > 4000000) return -1; continue; }
        if (w < 0) return -1;
        off += w;
    }
    return 0;
}

/* Read fd until EOF; returns byte count or -1 (no EOF
 * within the spin bound, or a read error). */
static long read_all(long fd, char *buf, long cap)
{
    long total = 0;
    long spins = 0;
    while (spins++ < 4000000) {
        if (total == cap) return -1;
        long r = sys_read(fd, buf + total, cap - total);
        if (r == E_WOULDBLOCK) continue;
        if (r == 0) return total;
        if (r < 0) return -1;
        total += r;
    }
    return -1;
}

static int make_file(const char *name, const char *data, long len)
{
    long fd = sys_fcreate(name);
    if (fd < 0) return -1;
    int rc = write_all(fd, data, len);
    if (sys_close(fd) != 0) rc = -1;
    return rc;
}

static int streq_n(const char *a, const char *b, long n)
{
    for (long i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* cat.elf <payload_name> | wc.elf, harness reads wc. */
static void run_cat_wc(const char *payload_name, const char *expect,
                       const char *check_name)
{
    long P[2], Q[2];
    if (sys_pipe(P) != 0 || sys_pipe(Q) != 0) { check(0, check_name); return; }

    long catmap[3]; catmap[0] = 1; catmap[1] = 1; catmap[2] = P[1];
    char cmd[80];
    long ci = 0;
    const char *pre = "cat.elf ";
    while (*pre) cmd[ci++] = *pre++;
    const char *nm = payload_name;
    while (*nm) cmd[ci++] = *nm++;
    cmd[ci] = 0;
    long cpid = sys_exec_inherit("cat.elf", cmd, catmap);

    long wcmap[5];
    wcmap[0] = 2; wcmap[1] = 0; wcmap[2] = P[0]; wcmap[3] = 1; wcmap[4] = Q[1];
    long wpid = sys_exec_inherit("wc.elf", "wc.elf", wcmap);
    if (cpid <= 0 || wpid <= 0) { check(0, check_name); return; }

    /* The harness's own aliases of P-write and Q-write
     * would keep the readers from EOF; drop all three. */
    sys_close(P[1]);
    sys_close(P[0]);
    sys_close(Q[1]);

    char out[64];
    long n = read_all(Q[0], out, sizeof out);
    sys_close(Q[0]);
    long cexit = sys_wait(cpid);
    long wexit = sys_wait(wpid);
    check(n == slen(expect) && streq_n(out, expect, n)
          && cexit == 0 && wexit == 0, check_name);
}

static char big[2000];

void _start(long argc, char **argv)
{
    (void)argc; (void)argv;

    /* Payloads. big: 200 lines of "word word\n". */
    long bi = 0;
    for (int i = 0; i < 200; i++) {
        const char *line = "word word\n";
        while (*line) big[bi++] = *line++;
    }

    if (make_file("tt_empty.txt", "", 0) != 0) check(0, "make empty");
    if (make_file("tt_nonl.txt", "alpha beta\ngamma", 16) != 0) check(0, "make nonl");
    if (make_file("tt_big.txt", big, 2000) != 0) check(0, "make big");

    run_cat_wc("tt_empty.txt", "0 0 0\n", "cat-wc empty");
    run_cat_wc("tt_nonl.txt", "1 3 16\n", "cat-wc no-trailing-newline");
    run_cat_wc("tt_big.txt", "200 400 2000\n", "cat-wc big");

    /* (2) cat stdin round trip, 2000 bytes, byte-exact. */
    {
        long R[2], S[2];
        int ok = 0;
        if (sys_pipe(R) == 0 && sys_pipe(S) == 0) {
            long map[5];
            map[0] = 2; map[1] = 0; map[2] = R[0]; map[3] = 1; map[4] = S[1];
            long pid = sys_exec_inherit("cat.elf", "cat.elf", map);
            sys_close(R[0]);
            sys_close(S[1]);   /* harness alias would block EOF */
            if (pid > 0
                    && write_all(R[1], big, 2000) == 0) {
                sys_close(R[1]);   /* delivers cat's EOF */
                static char got[2048];
                long n = read_all(S[0], got, sizeof got);
                ok = (n == 2000 && streq_n(got, big, 2000)
                      && sys_wait(pid) == 0);
            }
            sys_close(S[0]);
        }
        check(ok, "cat roundtrip big");
    }

    /* (3) cat on a nonexistent file: exit code 2. */
    {
        long pid = sys_exec("cat.elf", "cat.elf tt_nosuch.txt");
        check(pid > 0 && sys_wait(pid) == 2, "cat missing exit 2");
    }

    puts1(fails == 0 ? "tooltest: ALL PASS\r\n" : "tooltest: FAILURES\r\n");
    sys_exit(fails == 0 ? 0 : 1);
}
