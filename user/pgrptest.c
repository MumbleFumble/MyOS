/*
 * pgrptest.elf - Phase 18 inc.4 process-group harness.
 *
 * Orchestrator cases:
 *  1. Inheritance: the harness's pgid is its parent's
 *     (the shell's) group; a child's pgid equals it;
 *     a grandchild (checked inside the "mid" mode)
 *     likewise.
 *  2. setpgid: a task makes itself a group leader and
 *     rejoins ("mover" mode); the harness moves a
 *     child to leadership and back; refusals (bad
 *     pid, nonexistent group, DEAD target, bad getpgid
 *     pid) each return -1 and change nothing.
 *  3. Group kill: two spinners in one group both
 *     collect 4096+sig from ONE kill(-pgid, sig); a
 *     spinner in the harness's own group is untouched.
 *  4. Edges: nonexistent group -1; invalid signal -1
 *     with members provably alive afterwards; a group
 *     whose members all died is empty (-1).
 *
 * "pgrp" mode: after a settle delay, prints
 * "pgrp pid=<pid> pgid=<pgid>" -- the host test uses
 * it to observe the shell's pipeline grouping.
 *
 * Spawn budget: ~11 children. Inside MAX_TASKS 32.
 */
#include "../src/lib/syscall.h"

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sys_write(1, s, slen(s)); }
static void putn(long v)
{
    char buf[16]; int i = 15;
    buf[i] = 0;
    if (v == 0) buf[--i] = '0';
    while (v > 0) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    puts1(buf + i);
}

static int fails = 0;
static void check(int cond, const char *name)
{
    if (cond) {
        puts1("pgrptest: ok ");
        puts1(name);
        puts1("\r\n");
    } else {
        puts1("pgrptest: FAIL ");
        puts1(name);
        puts1("\r\n");
        fails++;
    }
}

static long spawn_spinner(void)
{
    return sys_exec("hostileprobe.elf", "hostileprobe.elf spin");
}

void _start(long argc, char **argv)
{
    if (argc >= 2 && argv[1][0] == 'm' && argv[1][1] == 'i') {
        /* mid: our child must inherit OUR pgid. */
        long leaf = sys_exec("argtest.elf", "argtest.elf x");
        long ok = leaf > 0 && sys_getpgid(leaf) == sys_getpgid(0);
        sys_wait(leaf);
        sys_exit(ok ? 0 : 1);
    }
    if (argc >= 2 && argv[1][0] == 'm' && argv[1][1] == 'o') {
        /* mover: become a leader, then rejoin the
         * group we came from. */
        long g0 = sys_getpgid(0), me = sys_getpid();
        if (sys_setpgid(0, me) != 0) sys_exit(1);
        if (sys_getpgid(0) != me) sys_exit(2);
        if (sys_setpgid(0, g0) != 0) sys_exit(3);
        if (sys_getpgid(0) != g0) sys_exit(4);
        sys_exit(0);
    }
    if (argc >= 2 && argv[1][0] == 'p') {
        /* pgrp: settle (let a spawning shell finish
         * any post-spawn setpgid), then report. */
        volatile long sink = 0;
        for (long k = 0; k < 50000000L; k++) sink += k;
        (void)sink;
        puts1("pgrp pid=");
        putn(sys_getpid());
        puts1(" pgid=");
        putn(sys_getpgid(0));
        puts1("\r\n");
        sys_exit(0);
    }

    long G = sys_getpgid(0);
    check(G > 0 && G == sys_getpgid(sys_getpid()), "inherit self");

    long child = sys_exec("argtest.elf", "argtest.elf one two");
    check(sys_wait(child) == 0 && sys_getpgid(child) == G,
          "child inherit");
    long mid = sys_exec("pgrptest.elf", "pgrptest.elf mid");
    check(sys_wait(mid) == 0, "grandchild inherit");
    long mover = sys_exec("pgrptest.elf", "pgrptest.elf mover");
    check(sys_wait(mover) == 0, "setpgid self move");

    long S = spawn_spinner();
    check(sys_setpgid(S, S) == 0 && sys_getpgid(S) == S,
          "setpgid child leader");
    check(sys_setpgid(S, G) == 0 && sys_getpgid(S) == G,
          "setpgid child rejoin");
    check(sys_kill(S, SIGKILL) == 0 &&
          sys_wait(S) == EXIT_SIGNAL_BASE + SIGKILL,
          "moved child kill");

    check(sys_setpgid(9999, 9999) == -1, "setpgid badpid");
    long S2 = spawn_spinner();
    check(sys_setpgid(S2, 4242) == -1 && sys_getpgid(S2) == G,
          "setpgid no such group");
    sys_kill(S2, SIGKILL); sys_wait(S2);
    long D0 = sys_exec("argtest.elf", "argtest.elf x");
    sys_wait(D0);
    check(sys_setpgid(D0, D0) == -1, "setpgid dead");
    check(sys_getpgid(9999) == -1, "getpgid badpid");

    long A = spawn_spinner(), B = spawn_spinner(), C = spawn_spinner();
    sys_setpgid(A, A);
    sys_setpgid(B, A);
    check(sys_kill(-A, SIGTERM) == 0, "group kill rc");
    check(sys_wait(A) == EXIT_SIGNAL_BASE + SIGTERM, "group A 4111");
    check(sys_wait(B) == EXIT_SIGNAL_BASE + SIGTERM, "group B 4111");
    check(sys_kill(C, SIGKILL) == 0 &&
          sys_wait(C) == EXIT_SIGNAL_BASE + SIGKILL,
          "other group untouched");

    check(sys_kill(-4242, SIGKILL) == -1, "group nonexistent");
    long D = spawn_spinner();
    sys_setpgid(D, D);
    check(sys_kill(-D, 99) == -1, "group invalid sig");
    check(sys_kill(-D, SIGKILL) == 0 &&
          sys_wait(D) == EXIT_SIGNAL_BASE + SIGKILL,
          "group member alive after invalid");
    check(sys_kill(-D, SIGKILL) == -1, "group empty after death");

    if (fails == 0) puts1("pgrptest: ALL PASS\r\n");
    else puts1("pgrptest: FAILURES\r\n");
    sys_exit(fails ? 1 : 0);
}
