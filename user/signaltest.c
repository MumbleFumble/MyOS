/*
 * signaltest.elf - Phase 18 inc.3 signal vocabulary +
 * wait-encoding harness.
 *
 * Cases (orchestrator mode, no arguments):
 *  1-3. A spawned spinner (hostileprobe.elf spin) is
 *     killed with each signal of the vocabulary and
 *     collects exactly EXIT_SIGNAL_BASE + sig:
 *     SIGKILL 4105, SIGTERM 4111, SIGINT 4098 - three
 *     distinct values.
 *  4. Invalid signals (0, 99, -1) each return -1 and
 *     leave the target untouched, proven by a
 *     subsequent valid SIGTERM kill collecting 4111.
 *  5. kill(self, SIGTERM) collects 4111 (the selfterm
 *     mode below; it must never return).
 *  6. A valid signal to a nonexistent pid returns -1.
 *
 * Spawn budget: 5 children - well inside MAX_TASKS 32.
 */
#include "../src/lib/syscall.h"

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sys_write(1, s, slen(s)); }

static int fails = 0;
static void check(int cond, const char *name)
{
    if (cond) {
        puts1("signaltest: ok ");
        puts1(name);
        puts1("\r\n");
    } else {
        puts1("signaltest: FAIL ");
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
    if (argc >= 2 && argv[1][0] == 's' && argv[1][1] == 'e') {
        /* selfterm: kill(self, SIGTERM) must not return.
         * (The shell also execs this mode to prove a
         * signal death surfaces in $?.) */
        sys_kill(sys_getpid(), SIGTERM);
        puts1("signaltest: FAIL selfterm returned\r\n");
        sys_exit(9);
    }

    long p;
    p = spawn_spinner();
    check(p > 0, "spawn sigkill");
    check(sys_kill(p, SIGKILL) == 0 &&
          sys_wait(p) == EXIT_SIGNAL_BASE + SIGKILL, "SIGKILL 4105");

    p = spawn_spinner();
    check(p > 0, "spawn sigterm");
    check(sys_kill(p, SIGTERM) == 0 &&
          sys_wait(p) == EXIT_SIGNAL_BASE + SIGTERM, "SIGTERM 4111");

    p = spawn_spinner();
    check(p > 0, "spawn sigint");
    check(sys_kill(p, SIGINT) == 0 &&
          sys_wait(p) == EXIT_SIGNAL_BASE + SIGINT, "SIGINT 4098");

    p = spawn_spinner();
    check(p > 0, "spawn invalid");
    check(sys_kill(p, 0) == -1 && sys_kill(p, 99) == -1 &&
          sys_kill(p, -1) == -1, "invalid sigs -1");
    check(sys_kill(p, SIGTERM) == 0 &&
          sys_wait(p) == EXIT_SIGNAL_BASE + SIGTERM,
          "invalid sig untouched");

    p = sys_exec("signaltest.elf", "signaltest.elf selfterm");
    check(sys_wait(p) == EXIT_SIGNAL_BASE + SIGTERM, "selfterm 4111");

    check(sys_kill(9999, SIGKILL) == -1, "badpid");

    if (fails == 0) puts1("signaltest: ALL PASS\r\n");
    else puts1("signaltest: FAILURES\r\n");
    sys_exit(fails ? 1 : 0);
}
