/*
 * fdchild.elf - Phase 17 inc.5 echo child.
 *
 * Copies fd 0 to fd 1 until EOF, then closes fd 1 (the
 * close is what delivers EOF downstream when this slot
 * holds the last alias of a pipe write end) and exits 7.
 * Distinct exit codes name the failure: 8 close failed,
 * 9 read error, 10 write error. Spins on E_WOULDBLOCK:
 * the pipe layer is non-blocking by design.
 */
#include "../src/lib/syscall.h"

void _start(long argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[64];
    for (;;) {
        long r = sys_read(0, buf, sizeof buf);
        if (r == E_WOULDBLOCK) continue;
        if (r == 0) break;
        if (r < 0) sys_exit(9);
        long off = 0;
        while (off < r) {
            long w = sys_write(1, buf + off, r - off);
            if (w == E_WOULDBLOCK) continue;
            if (w < 0) sys_exit(10);
            off += w;
        }
    }
    if (sys_close(1) != 0) sys_exit(8);
    sys_exit(7);
}
