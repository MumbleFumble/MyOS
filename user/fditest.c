/*
 * fditest.elf - Phase 17 inc.5 fd-inheritance harness.
 *
 * (i)+(ii) Pipe round trip through a spawned child with
 * cross-task EOF semantics: pipes A (parent -> child) and
 * B (child -> parent); the child is spawned with A-read as
 * its fd 0 and B-write as its fd 1. The parent writes the
 * payload, closes its A-write alias (child drains, sees
 * EOF), closes its own B-write alias, then reads B: after
 * the echoed bytes are drained a read must return
 * E_WOULDBLOCK, NOT EOF - the child's inherited alias of
 * the same write-end description still counts. EOF may
 * arrive only when the child closes its fd 1 (its last
 * alias), which runs the backend close exactly once.
 * (iii) Invalid inheritance requests (closed parent fd,
 * duplicate child target, out-of-range child fd) each
 * return -1 and a bad map pointer returns EFAULT, with no
 * child spawned by any of them.
 * (iv) A plain sys_exec child still gets the pinned
 * console table (fdprobe.elf reports it).
 */
#include "../src/lib/syscall.h"

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sys_write(1, s, slen(s)); }

static int fails = 0;
static void check(int cond, const char *name)
{
    if (cond) {
        puts1("fditest: ok ");
        puts1(name);
        puts1("\r\n");
    } else {
        puts1("fditest: FAIL ");
        puts1(name);
        puts1("\r\n");
        fails++;
    }
}

void _start(long argc, char **argv)
{
    (void)argc; (void)argv;
    static const char payload[] = "inherit-echo-42";
    const long plen = 15;

    long A[2], B[2];
    if (sys_pipe(A) != 0 || sys_pipe(B) != 0) {
        puts1("fditest: FAIL pipe create\r\n");
        sys_exit(1);
    }

    /* Map: count=2, {0 <- A read end, 1 <- B write end}. */
    long map[5];
    map[0] = 2; map[1] = 0; map[2] = A[0]; map[3] = 1; map[4] = B[1];
    long pid = sys_exec_inherit("fdchild.elf", "fdchild.elf", map);
    check(pid > 0, "spawn inherit");

    sys_write(A[1], payload, plen);
    sys_close(A[1]);   /* child drains A, then sees EOF */
    sys_close(B[1]);   /* parent's B-write alias gone; the
                          child's alias must keep B from EOF */

    char got[64];
    long total = 0, saw_block = 0, saw_eof = 0;
    long spins = 0;
    while (spins++ < 4000000) {
        long r = sys_read(B[0], got + total, sizeof got - total);
        if (r == E_WOULDBLOCK) { saw_block = 1; continue; }
        if (r == 0) { saw_eof = 1; break; }
        if (r < 0) break;
        total += r;
    }
    int same = (total == plen);
    for (long i = 0; same && i < plen; i++)
        if (got[i] != payload[i]) same = 0;
    check(same, "roundtrip byte-exact");
    check(saw_block && saw_eof, "eof only at child close");
    check(sys_wait(pid) == 7, "child exit 7");

    /* (iii) refusals: no child, no moved refcounts. */
    long bad1[3]; bad1[0] = 1; bad1[1] = 0; bad1[2] = 9;      /* closed parent fd */
    check(sys_exec_inherit("fdchild.elf", "fdchild.elf", bad1) == -1,
          "refuse closed parent fd");
    long bad2[5]; bad2[0] = 2; bad2[1] = 0; bad2[2] = A[0];
    bad2[3] = 0; bad2[4] = B[0];                              /* duplicate child 0 */
    check(sys_exec_inherit("fdchild.elf", "fdchild.elf", bad2) == -1,
          "refuse duplicate child fd");
    long bad3[3]; bad3[0] = 1; bad3[1] = 16; bad3[2] = A[0];  /* child fd range */
    check(sys_exec_inherit("fdchild.elf", "fdchild.elf", bad3) == -1,
          "refuse child fd 16");
    check(sys_exec_inherit("fdchild.elf", "fdchild.elf",
                           (const long *)0x1000) == EFAULT,
          "bad map pointer EFAULT");

    /* (iv) default spawn: console table, pinned. Pids are
     * minted sequentially and nothing else spawns during
     * this program, so the probe's pid also proves the
     * four refusals above spawned nothing: it must be
     * exactly pid + 1. */
    long pid2 = sys_exec("fdprobe.elf", "fdprobe.elf");
    check(pid2 == pid + 1, "refusals spawned nothing");
    check(pid2 > 0 && sys_wait(pid2) == 0, "default console child");

    puts1(fails == 0 ? "fditest: ALL PASS\r\n" : "fditest: FAILURES\r\n");
    sys_exit(fails == 0 ? 0 : 1);
}
