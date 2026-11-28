/*
 * killtest.elf - Phase 18 inc.2 SYS_KILL + fd-teardown
 * harness.
 *
 * Cases (orchestrator mode, no arguments):
 *  1. A spawned spinner (hostileprobe.elf spin) is
 *     killed and collected as exactly EXIT_KILLED 4001.
 *  2. kill of a nonexistent pid returns -1.
 *  3. kill of an already-DEAD pid returns -1 and the
 *     dead task's collected status is unchanged.
 *  4. kill(self) collects 4001 (the selfkill mode
 *     below; it must never return from sys_kill).
 *  5. EOF-on-kill: fdchild.elf inherits a pipe write
 *     end; while it lives a read on the harness's end
 *     returns E_WOULDBLOCK (not EOF); after SYS_KILL
 *     the read returns EOF, because the kill's fd
 *     teardown released the writer's last alias; the
 *     writer collects 4001.
 *  6. A task blocked in SYS_WAIT (the waiter mode
 *     below, waiting on a spinner) is killed and
 *     collected as 4001; it must never resume (it
 *     prints WAITER-RESURRECTED if its wait ever
 *     returns, which the host test greps for == 0).
 *
 * Spawn budget: 6 children (spinner, argtest,
 * selfkill, fdchild, spinner, waiter) - well inside
 * MAX_TASKS 32 for the whole script.
 */
#include "../src/lib/syscall.h"

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sys_write(1, s, slen(s)); }

static int fails = 0;
static void check(int cond, const char *name)
{
    if (cond) {
        puts1("killtest: ok ");
        puts1(name);
        puts1("\r\n");
    } else {
        puts1("killtest: FAIL ");
        puts1(name);
        puts1("\r\n");
        fails++;
    }
}

void _start(long argc, char **argv)
{
    if (argc >= 2 && argv[1][0] == 's') {
        /* selfkill: kill(self) must never return. */
        sys_kill(sys_getpid());
        puts1("killtest: FAIL selfkill returned\r\n");
        sys_exit(9);
    }
    if (argc >= 2 && argv[1][0] == 'w') {
        /* waiter: spawn our OWN spinner and block in
         * SYS_WAIT on it (SYS_WAIT only waits on the
         * caller's children). If the wait ever returns
         * after we are killed, the resurrection marker
         * lands on the console. */
        long s = sys_exec("hostileprobe.elf", "hostileprobe.elf spin");
        sys_wait(s);
        puts1("killtest: WAITER-RESURRECTED\r\n");
        sys_exit(3);
    }

    /* 1. Killed spinner collects exactly 4001. */
    long spin = sys_exec("hostileprobe.elf", "hostileprobe.elf spin");
    check(spin > 0, "spawn spinner");
    check(sys_kill(spin) == 0, "spinner kill rc");
    check(sys_wait(spin) == EXIT_KILLED, "spinner 4001");

    /* 2. Nonexistent pid. */
    check(sys_kill(9999) == -1, "badpid");

    /* 3. Already-DEAD pid: -1, status unchanged. */
    long done = sys_exec("argtest.elf", "argtest.elf one two");
    check(sys_wait(done) == 0, "argtest exit 0");
    check(sys_kill(done) == -1 && sys_wait(done) == 0, "deadpid");

    /* 4. kill(self) collects 4001. */
    long self = sys_exec("killtest.elf", "killtest.elf selfkill");
    check(sys_wait(self) == EXIT_KILLED, "selfkill 4001");

    /* 5. EOF-on-kill (fdchild reuses the inc.5 fixture). */
    long P[2], Q[2];
    if (sys_pipe(P) != 0 || sys_pipe(Q) != 0) {
        puts1("killtest: FAIL pipe create\r\n");
        sys_exit(1);
    }
    long map[5];
    map[0] = 2; map[1] = 0; map[2] = P[0]; map[3] = 1; map[4] = Q[1];
    long writer = sys_exec_inherit("fdchild.elf", "fdchild.elf", map);
    check(writer > 0, "spawn writer");
    sys_close(Q[1]);            /* harness's own write alias */
    sys_close(P[0]);
    char buf[8];
    check(sys_read(Q[0], buf, 1) == -2, "no eof before kill");
    check(sys_kill(writer) == 0, "writer kill rc");
    check(sys_read(Q[0], buf, 1) == 0, "eof after kill");
    check(sys_wait(writer) == EXIT_KILLED, "writer 4001");

    /* 6. Killed WAITING task never resumes. The waiter
     * spawns its own spinner and blocks waiting on it.
     * Pids are assigned sequentially and nothing else
     * spawns in between, so the waiter's spinner is
     * waiter+1; the harness cannot collect it (not its
     * child), so its death is proven by a second kill
     * returning -1. */
    long waiter = sys_exec("killtest.elf", "killtest.elf waiter");
    /* Give the waiter time to enter SYS_WAIT; the
     * assertions hold even if it is still READY, and
     * the never-resume mechanism itself is state-based
     * (see the inc.2 report's wake-path audit). */
    volatile long sink = 0;
    for (long k = 0; k < 100000000L; k++) sink += k;
    (void)sink;
    check(sys_kill(waiter) == 0, "waiting kill rc");
    check(sys_wait(waiter) == EXIT_KILLED, "waiting 4001");
    check(sys_kill(waiter + 1) == 0, "waiters spinner kill rc");
    check(sys_kill(waiter + 1) == -1, "waiters spinner dead");

    if (fails == 0) puts1("killtest: ALL PASS\r\n");
    else puts1("killtest: FAILURES\r\n");
    sys_exit(fails ? 1 : 0);
}
